"""Deterministic fill-ledger replay with moving-average cost and fee allocation."""
from decimal import Decimal


def replay_fills(fills):
    positions, exits, errors = {}, {}, []
    buy = sell = fees = Decimal(0)
    for f in fills:  # caller orders by fill timestamp, then monotonic fill id
        key = (f.get('mode'),f.get('instance'),
               None if str(f.get('mode')).lower()=='live' else f.get('session_id'),
               f.get('condition_id'),f.get('token_id'))
        p = positions.setdefault(key, {**f, 'shares':Decimal(0), 'cost':Decimal(0),
                                       'entry_fees':Decimal(0)})
        price, shares, fee = (Decimal(str(f.get(k) or 0)) for k in ('price','size_shares','fee_usd'))
        if (not all(v.is_finite() for v in (price,shares,fee)) or
                shares <= 0 or not 0 <= price <= 1):
            errors.append('Invalid fill '+str(f.get('id')))
            continue
        fees += fee
        p['last_ts'] = f.get('ts')
        if f['direction'] == 'buy':
            buy += price*shares
            p['cost'] += price*shares
            p['entry_fees'] += fee
            p['shares'] += shares
        elif f['direction'] == 'sell':
            sell += price*shares
            held = p['shares']
            matched = min(shares,held)
            cost = p['cost']*matched/held if held else Decimal(0)
            allocated_fee = p['entry_fees']*matched/held if held else Decimal(0)
            e = exits.setdefault(f['client_order_id'], {**f, 'shares':Decimal(0),
                'gross_proceeds':Decimal(0),'cost_basis':Decimal(0),'entry_cost':Decimal(0),
                'fees':Decimal(0),'realized_pnl':Decimal(0),'matched':True})
            e['shares'] += shares
            e['gross_proceeds'] += price*shares
            e['entry_cost'] += cost
            e['cost_basis'] += cost+allocated_fee
            e['fees'] += fee+allocated_fee
            e['realized_pnl'] += price*shares-cost-allocated_fee-fee
            if shares-matched > Decimal('0.000001'):
                e['matched'] = False
                errors.append('Sell exceeds recorded holdings: '+str(f['client_order_id']))
            p['shares'] -= matched
            p['cost'] -= cost
            p['entry_fees'] -= allocated_fee
        else:
            errors.append('Unknown fill direction: '+str(f.get('id')))
    def clean(row):
        return {k:float(v) if isinstance(v,Decimal) else v for k,v in row.items()}
    return {'positions':[clean(p) for p in positions.values() if p['shares'] > Decimal('0.000001')],
            'exits':[clean(e) for e in exits.values()], 'buy_value':float(buy),
            'sell_value':float(sell), 'fees':float(fees), 'errors':errors}
