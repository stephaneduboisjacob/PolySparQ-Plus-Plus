"""Read-only performance diagnosis from complete recorded market-side exposures.

No fitted thresholds, hypothetical fills, or automatic trading changes. A sample
is a mode/instance/session/market/token exposure, not an individual partial exit.
LIVE inventory spans process sessions, matching the core accounting ledger.
"""
from collections import defaultdict
from datetime import datetime
from decimal import Decimal, InvalidOperation
import math


def timestamp_ms(value):
    if isinstance(value, datetime):
        return int(value.timestamp()*1000)
    return int(value)


def completed_exposures(fills):
    groups = defaultdict(list)
    for f in fills:
        mode = str(f.get('mode', '')).lower()
        key = (mode, f.get('instance'), None if mode == 'live' else f.get('session_id'),
               f.get('condition_id'), f.get('token_id'))
        groups[key].append(f)
    completed, excluded = [], defaultdict(int)
    for key, rows in groups.items():
        rows = sorted(rows, key=lambda f: (timestamp_ms(f['ts']), f.get('id', 0)))
        balance = cost = proceeds = buy_fees = sell_fees = bought = Decimal(0)
        buys, sells, error = [], [], None
        for f in rows:
            try:
                price, shares, fee = [Decimal(str(f.get(k) or 0)) for k in ('price', 'size_shares', 'fee_usd')]
                if not all(x.is_finite() for x in (price, shares, fee)) or not 0 <= price <= 1 or shares <= 0:
                    raise ValueError('invalid fill')
            except (InvalidOperation, ValueError):
                error = 'invalid_fill'; break
            if f['direction'] == 'buy':
                balance += shares; bought += shares; cost += price*shares; buy_fees += fee
                buys.append(f)
            elif f['direction'] == 'sell':
                balance -= shares; proceeds += price*shares; sell_fees += fee
                sells.append(f)
                if balance < Decimal('-0.000001'):
                    error = 'unmatched_exit'; break
            else:
                error = 'invalid_direction'; break
        if error or not buys or not sells or balance > Decimal('0.000001'):
            excluded[error or ('open_or_missing_exit' if buys else 'unmatched_exit')] += 1
            continue
        first = buys[0]
        execution = {f.get('execution_mode') or 'unknown' for f in buys}
        sources = {f.get('source') or 'unknown' for f in buys}
        completed.append({
            'instance':key[1], 'mode':key[0], 'session_id':key[2],
            'condition_id':key[3], 'token_id':key[4], 'side':first.get('side_label'),
            'asset':first.get('asset'), 'horizon_seconds':first.get('horizon_seconds'),
            'first_entry_ms':timestamp_ms(first['ts']), 'last_entry_ms':timestamp_ms(buys[-1]['ts']),
            'exit_ms':timestamp_ms(sells[-1]['ts']), 'shares':float(bought),
            'entry_cost':float(cost), 'entry_fees':float(buy_fees), 'exit_fees':float(sell_fees),
            'gross_proceeds':float(proceeds), 'net':float(proceeds-cost-buy_fees-sell_fees),
            'entry_execution':next(iter(execution)) if len(execution)==1 else 'mixed',
            'entry_source':next(iter(sources)) if len(sources)==1 else 'mixed',
            'exit_sources':sorted({f.get('source') or 'unknown' for f in sells}),
            'entry_price':float(cost/bought),
            'entry_orders':len({f['client_order_id'] for f in buys}),
            'entry_order_ids':sorted({str(f['client_order_id']) for f in buys}),
            'exit_orders':len({f['client_order_id'] for f in sells}),
            'settled_outcome':int(sells[0]['price']) if all(
                f.get('source')=='settle' and f['price'] in (0,1) and
                f['price']==sells[0]['price'] for f in sells) else None})
    return completed, dict(excluded)


def metrics(rows):
    wins = [r['net'] for r in rows if r['net'] > 0]
    losses = [r['net'] for r in rows if r['net'] < 0]
    net = sum(r['net'] for r in rows)
    fees = sum(r['entry_fees']+r['exit_fees'] for r in rows)
    capital = sum(r['entry_cost']+r['entry_fees'] for r in rows)
    shares = sum(r['shares'] for r in rows)
    return {'samples':len(rows), 'wins':len(wins), 'losses':len(losses),
            'breakeven':len(rows)-len(wins)-len(losses), 'net':round(net,4),
            'gross':round(net+fees,4), 'fees':round(fees,4),
            'expectancy':net/len(rows) if rows else None,
            'average_win':sum(wins)/len(wins) if wins else None,
            'average_loss':sum(losses)/len(losses) if losses else None,
            'profit_factor':sum(wins)/abs(sum(losses)) if losses else None,
            'roi_pct':100*net/capital if capital else None,
            'cost_per_share':capital/shares if shares else None,
            'net_proceeds_per_share':sum(r['gross_proceeds']-r['exit_fees'] for r in rows)/shares if shares else None,
            'unique_markets':len({r['condition_id'] for r in rows}),
            'limited_sample':len({r['condition_id'] for r in rows}) < 30}


def performance_review(fills, since_ms, now_ms, cohort_since_ms=None):
    # Exclude future fills before reconstruction: this also supports deterministic replay.
    completed, excluded = completed_exposures([f for f in fills if timestamp_ms(f['ts']) <= now_ms])
    rows = [r for r in completed if since_ms <= r['exit_ms'] <= now_ms]
    def grouped(fields):
        buckets = defaultdict(list)
        for r in rows:
            buckets[tuple(r.get(k) or 'unknown' for k in fields)].append(r)
        return [{**dict(zip(fields, key)), **metrics(rs)} for key, rs in sorted(buckets.items())]
    for r in rows:
        lower = math.floor(r['entry_price']*10+1e-8)/10
        r['price_band'] = f'{lower:.2f}–{lower+.1:.2f}'
    clusters = defaultdict(list)
    for r in rows:
        clusters[(r['mode'],r['condition_id'],r['side'])].append(r)
    clustered = []
    for (mode,cid,side), rs in clusters.items():
        bots = sorted({r['instance'] for r in rs})
        if len(bots)>1 and sum(r['net'] for r in rs)<0:
            clustered.append({'mode':mode,'condition_id':cid,'side':side,'instances':bots,
                'asset':rs[0]['asset'],'horizon_seconds':rs[0]['horizon_seconds'],
                'exit_ms':max(r['exit_ms'] for r in rs),**metrics(rs)})
    cohorts = None
    if cohort_since_ms is not None:
        cohorts = {'since_ms':cohort_since_ms,
            'before':metrics([r for r in rows if r['last_entry_ms'] < cohort_since_ms]),
            'after':metrics([r for r in rows if r['first_entry_ms'] >= cohort_since_ms]),
            'straddling':metrics([r for r in rows if r['first_entry_ms'] < cohort_since_ms <= r['last_entry_ms']])}
    return {'server_ts':now_ms,'since_ms':since_ms,'sample_unit':'completed_market_side_exposure',
        'method':'Complete mode/instance/session/market/token exposures including every exit and recorded fee. LIVE spans sessions. Period selected by final exit time.',
        'limitations':['PAPER fills are simulated; recorded fee assumptions may differ from live venue charges.',
            'Repeated entries into the same market-side within a session form one sample.',
            'Shared markets and assets are correlated; sample count is not independent evidence.',
            'Before/after cohorts use entry timestamps, not verified effective config versions. No causal claim.'],
        'summary':metrics(rows),'instances':grouped(['instance','mode']),
        'entry_execution':grouped(['mode','entry_execution']),
        'entry_sources':grouped(['instance','mode','entry_source']),
        'entry_prices':grouped(['instance','mode','price_band']),
        'loss_clusters':sorted(clustered,key=lambda r:r['net'])[:20],
        'cohorts':cohorts,'excluded':excluded,
        'model_candidates':[{'order_id':r['entry_order_ids'][0], 'instance':r['instance'],
                             'asset':r['asset'], 'horizon_seconds':r['horizon_seconds'],
                             'side':r['side'], 'outcome':r['settled_outcome'],
                             'condition_id':r['condition_id']}
                            for r in rows if r['entry_source']=='WPRED' and
                            len(r['entry_order_ids'])==1 and r['settled_outcome'] is not None],
        'rolling':[{'hours':h,**metrics([r for r in completed if now_ms-h*3600000<=r['exit_ms']<=now_ms])}
                   for h in (1,6,24,168)],
        'recent':sorted(rows,key=lambda r:r['exit_ms'],reverse=True)[:100]}


def prediction_calibration(candidates, signal_payloads):
    """Match only exact order IDs and fully settled binary outcomes."""
    by_id = {str(p.get('client_order_id')):p for p in signal_payloads
             if isinstance(p,dict) and p.get('client_order_id')}
    groups = defaultdict(list)
    matched = []
    for c in candidates:
        payload = by_id.get(c['order_id'])
        if not payload:
            continue
        try:
            fair_up = float(payload['fair_up'])
            if not math.isfinite(fair_up) or not 0 < fair_up < 1:
                continue
            p = fair_up if c['side']=='UP' else 1-fair_up if c['side']=='DOWN' else None
            if p is None:
                continue
        except (KeyError, ValueError, TypeError):
            continue
        row = {'instance':c['instance'],'asset':c['asset'],'horizon_seconds':c['horizon_seconds'],
               'market':c['condition_id'],'predicted':p,'outcome':c['outcome']}
        matched.append(row)
        groups[(c['instance'],c['asset'],c['horizon_seconds'])].append(row)
    def aggregate(rs):
        n=len(rs)
        return {'samples':n,'distinct_markets':len({r['market'] for r in rs}),
                'mean_predicted':sum(r['predicted'] for r in rs)/n if n else None,
                'realized_win_rate':sum(r['outcome'] for r in rs)/n if n else None,
                'brier_score':sum((r['predicted']-r['outcome'])**2 for r in rs)/n if n else None,
                'limited_sample':len({r['market'] for r in rs})<30}
    return {'overall':aggregate(matched),
            'groups':[{'instance':key[0],'asset':key[1],'horizon_seconds':key[2],
                       **aggregate(rs)} for key,rs in sorted(groups.items())],
            'method':'WPRED fair_up at matched entry order; only fully settled 0/1 exposures. Predictions are uncalibrated estimates; Brier lower is better.'}
