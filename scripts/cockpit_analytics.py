"""Cockpit read models built from the existing, scoped fill accounting ledger."""
from collections import defaultdict
from datetime import datetime, timezone
import math

from accounting import replay_fills


def summarize(exits):
    values = [float(e['realized_pnl']) for e in exits]
    wins = [v for v in values if v > 0]
    losses = [v for v in values if v < 0]
    count = len(values)
    fees = sum(e['fees'] for e in exits)
    capital = sum(e['cost_basis'] for e in exits)
    shares = sum(e['shares'] for e in exits)
    # Wilson interval describes the observed profitable-exit fraction only.
    interval = None
    if count:
        p, z = len(wins)/count, 1.96
        center = (p + z*z/(2*count))/(1+z*z/count)
        radius = z*math.sqrt(p*(1-p)/count+z*z/(4*count*count))/(1+z*z/count)
        interval = [100*(center-radius), 100*(center+radius)]
    equity = peak = drawdown = 0.0
    for e in sorted(exits, key=lambda e: (e['ts'], e.get('id', 0))):
        equity += e['realized_pnl']
        peak = max(peak, equity)
        drawdown = max(drawdown, peak-equity)
    return {'net': round(sum(values), 4), 'gross': round(sum(values)+fees, 4),
            'fees': round(fees, 4), 'trades': count, 'settled': count, 'wins': len(wins), 'losses': len(losses), 'breakeven':count-len(wins)-len(losses),
            'wr': 100*len(wins)/count if count else None, 'win_rate_ci95': interval,
            'avg_entry': sum(e['entry_cost'] for e in exits)/shares if shares else None,
            'breakeven_wr': None, 'ev': sum(values)/count if count else None,
            'average_win': sum(wins)/len(wins) if wins else None,
            'average_loss': sum(losses)/len(losses) if losses else None,
            'profit_factor': sum(wins)/abs(sum(losses)) if losses else None,
            'roi_pct': 100*sum(values)/capital if capital else None,
            'max_drawdown_usd': drawdown}


def build_analytics(fills, since_ms, now_ms):
    ledger = replay_fills(fills)
    window = [e for e in ledger['exits'] if since_ms <= e['ts'] <= now_ms]
    exits = [e for e in window if e['matched']]
    def aggregate(field):
        groups = defaultdict(list)
        for e in exits:
            groups[e.get(field) or 'unknown'].append(e)
        return [{field: key, **summarize(rows)} for key, rows in sorted(groups.items())]
    daily_groups = defaultdict(list)
    reason_groups = defaultdict(list)
    for e in exits:
        day = datetime.fromtimestamp(e['ts']/1000, timezone.utc).date().isoformat()
        daily_groups[day].append(e)
        reason_groups[(e.get('instance') or 'unknown', e.get('source') or 'unknown')].append(e)
    return {'ok': True, 'server_ts': now_ms,
            'days_window': (now_ms-since_ms)/86_400_000,
            'method': 'Matched exit orders using moving-average entry cost and allocated entry/exit fees; paper scoped by session, LIVE by instance. Window selected by exit time.',
            'sample_unit': 'exit_order', 'summary': summarize(exits),
            'instances': aggregate('instance'), 'assets': aggregate('asset'),
            'modes': aggregate('mode'), 'execution': aggregate('execution_mode'),
            'exit_reasons': [{'instance': key[0], 'source': key[1], 'n': len(rows),
                             **summarize(rows)} for key, rows in sorted(reason_groups.items())],
            'daily': [{'day': day, **summarize(rows)} for day, rows in sorted(daily_groups.items())],
            'exits': sorted(exits, key=lambda e:e['ts'], reverse=True), 'buckets': [],
            'excluded_unmatched_exits': len(window)-len(exits),
            'ledger_errors': ledger['errors'][:100],
            'ledger_error_count': len(ledger['errors'])}


def live_position_marks(board, now_ms):
    """Executable bids only: exact market/side, fresh observed CLOB books."""
    marks = {}
    if board.get('stale') or board.get('snapshot_age_s', float('inf')) > 10:
        return marks
    for card in board.get('cards', []):
        age = card.get('book_age_s')
        if (card.get('period') != 'CURRENT' or not card.get('condition_id') or
                card.get('window_close', 0) <= now_ms or age is None or not 0 <= age <= 15):
            continue
        for side, key in [('UP','up'),('DOWN','down')]:
            bid, ask = card.get('polymarket_'+key+'_bid'), card.get('polymarket_'+key+'_price')
            if (isinstance(bid,(float,int)) and isinstance(ask,(float,int)) and
                    math.isfinite(bid) and math.isfinite(ask) and 0 < bid <= ask < 1):
                marks[(card['condition_id'],side)] = {'bid':bid,'ask':ask,'ts':now_ms-int(age*1000)}
    return marks
