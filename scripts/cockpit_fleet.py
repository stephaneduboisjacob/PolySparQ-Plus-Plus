"""Fleet management: explicit lifecycle operations and safe profile provisioning."""
import hashlib
import json
import math
import os
from pathlib import Path
import re
import tempfile
import time
import tomllib

# Mirrors src/config/config.hpp and Config::load. These are implemented execution controls.
EXECUTION_DEFAULTS = {'entry_order_mode':'maker',
    'maker_improve_ticks':0,'maker_reprice_ms':750,'maker_poll_ms':500,
    'maker_max_age_ms':5000,'maker_cancel_before_close_ms':3000}


RECYCLER_DEFAULTS={'recycler_mode':'off','recycler_entry_price':.40,'recycler_target_price':.55,
    'recycler_entry_min_price':.25,'recycler_entry_max_price':.65,
    'recycler_target_profit_pct':15.,'recycler_momentum_lookback_secs':10,'recycler_min_momentum_ticks':5,
    'recycler_stop_ticks':0,'recycler_max_hold_secs':0,'recycler_entry_cutoff_secs':300,
    'recycler_max_cycles':15,'recycler_lookback_secs':30,
    'recycler_min_range':.08,'recycler_max_window_loss':3.,'recycler_max_session_loss':10.,
    'recycler_settlement_entry_enabled':False,
    'recycler_settlement_entry_secs':15,'recycler_settlement_entry_min_price':.85,
    'recycler_settlement_entry_max_price':.98,'recycler_settlement_only':False}
EXECUTION_DEFAULTS.update(RECYCLER_DEFAULTS)

RECYCLER_ENGINE_DEFAULTS={
    'recycler_mode':'off','recycler_maniac_mode':False,
    'recycler_entry_price':.40,'recycler_target_price':.55,
    'recycler_entry_min_price':.25,'recycler_entry_max_price':.65,
    'recycler_target_profit_pct':20.,'recycler_momentum_lookback_secs':10,
    'recycler_min_momentum_ticks':5,'recycler_stop_ticks':0,
    'recycler_max_hold_secs':0,'recycler_entry_cutoff_secs':300,
    'recycler_max_cycles':15,'recycler_lookback_secs':120,
    'recycler_min_range':.08,'recycler_max_window_loss':0.,
    'recycler_max_session_loss':0.,'recycler_min_settlement_edge_pct':10.,
    'recycler_reference_max_age_ms':3000,
    'recycler_late_risk_exit_enabled':True,'recycler_late_risk_min_prob':.35,
    'recycler_late_risk_bid_loss_pct':30.,'recycler_late_target_6m_pct':15.,
    'recycler_late_target_4m_pct':10.,'recycler_late_target_2m_pct':5.,
    'recycler_late_maker_secs':120,'recycler_late_taker_secs':60,
    'recycler_late_taker_exit_enabled':True,'recycler_settlement_entry_enabled':False,
    'recycler_settlement_entry_secs':15,'recycler_settlement_entry_min_price':.90,
    'recycler_settlement_entry_max_price':.98,'recycler_settlement_only':False,
    'recycler_entry_open_grace_secs':0,'recycler_first_entry_scale_pct':100.,
    'recycler_late_taker_min_bid':0.,
}

def validate_recycler(settings,mode,market=None):
    c={**RECYCLER_ENGINE_DEFAULTS,**(settings or {})}
    if market is None: market={}
    if not isinstance(market,dict):
        raise ValueError('Recycler market-window configuration is invalid.')
    for key,default in RECYCLER_ENGINE_DEFAULTS.items():
        value=c[key]
        valid=(isinstance(value,bool) if isinstance(default,bool) else
               isinstance(value,int) and not isinstance(value,bool) if isinstance(default,int) else
               isinstance(value,(int,float)) and not isinstance(value,bool) and math.isfinite(value) if isinstance(default,float) else
               isinstance(value,str))
        if not valid:
            raise ValueError(f'{key} has an invalid type or non-finite value.')
    if c['recycler_mode'] not in ('off','sniper','range'):
        raise ValueError('Recycler mode must be off, sniper, or range.')
    if c['recycler_mode']!='off' and mode!='paper':
        raise ValueError('Recycler supports PAPER only; LIVE passive sell/reconciliation support is unavailable.')
    maniac=c['recycler_maniac_mode']
    max_cycles=100 if maniac else 15
    max_cutoff=86400 if maniac else 600
    max_open_grace=3600 if maniac else 600
    if not (0.10 <= c['recycler_entry_price'] < c['recycler_target_price'] <= 0.90):
        raise ValueError('Recycler entry and target prices are outside the engine range.')
    if not (0.01 <= c['recycler_entry_min_price'] < c['recycler_entry_max_price'] <= 0.99):
        raise ValueError('Recycler entry price band is invalid.')
    if not (5 <= c['recycler_target_profit_pct'] <= 50 and
            c['recycler_late_target_6m_pct'] <= c['recycler_target_profit_pct']):
        raise ValueError('Recycler profit targets must be ordered and within 5%–50%.')
    if not (1 <= c['recycler_momentum_lookback_secs'] <= 10 and
            1 <= c['recycler_min_momentum_ticks'] <= 10 and
            0 <= c['recycler_stop_ticks'] <= 30 and
            0 <= c['recycler_max_hold_secs'] <= 600 and
            10 <= c['recycler_lookback_secs'] <= 300 and
            0.02 <= c['recycler_min_range'] <= 0.40):
        raise ValueError('Recycler entry, hold, or range settings are outside the engine limits.')
    if not (1 <= c['recycler_max_cycles'] <= max_cycles and
            1 <= c['recycler_entry_cutoff_secs'] <= max_cutoff and
            0 <= c['recycler_entry_open_grace_secs'] <= max_open_grace and
            1 <= c['recycler_first_entry_scale_pct'] <= 100):
        raise ValueError('Recycler cycle timing or sizing is outside the engine limits.')
    if not (0 <= c['recycler_max_window_loss'] <= 100 and
            0 <= c['recycler_max_session_loss'] <= 1000):
        raise ValueError('Recycler loss limits are outside the engine range.')
    if (c['recycler_max_window_loss'] > 0 and
            c['recycler_max_session_loss'] > 0 and
            c['recycler_max_window_loss'] > c['recycler_max_session_loss']):
        raise ValueError('Recycler window loss cap exceeds session loss cap.')
    if not (0 <= c['recycler_min_settlement_edge_pct'] <= 50 and
            100 <= c['recycler_reference_max_age_ms'] <= 30000):
        raise ValueError('Recycler settlement-reference controls are outside the engine limits.')
    if not (0 <= c['recycler_late_risk_min_prob'] <= 1 and
            0 <= c['recycler_late_risk_bid_loss_pct'] <= 100 and
            0 <= c['recycler_late_target_6m_pct'] <= 50 and
            0 <= c['recycler_late_target_4m_pct'] <= c['recycler_late_target_6m_pct'] and
            0 <= c['recycler_late_target_2m_pct'] <= c['recycler_late_target_4m_pct']):
        raise ValueError('Recycler late-exit risk and profit thresholds are inconsistent.')
    if not (10 <= c['recycler_late_taker_secs'] <= 120 and
            c['recycler_late_taker_secs'] < c['recycler_late_maker_secs'] <= 360 and
            0 <= c['recycler_late_taker_min_bid'] <= 0.99):
        raise ValueError('Recycler late maker/taker exit timing is inconsistent.')
    if not (1 <= c['recycler_settlement_entry_secs'] <= 120):
        raise ValueError('Recycler settlement-entry window must be between 1 and 120 seconds.')
    if not (0.85 <= c['recycler_settlement_entry_min_price'] < c['recycler_settlement_entry_max_price'] <= 0.98):
        raise ValueError('Recycler settlement-entry price band must be within 0.85–0.98 and ordered.')
    if c['recycler_settlement_entry_enabled'] and c['recycler_mode']=='off':
        raise ValueError('Recycler settlement entry requires sniper or range mode.')
    if c['recycler_settlement_only'] and not c['recycler_settlement_entry_enabled']:
        raise ValueError('Settlement-only mode requires settlement entries to be enabled.')
    if c['recycler_mode']=='off':
        return

    raw_horizons=market.get('universe_horizons_seconds',[300,900])
    if (not isinstance(raw_horizons,list) or
            any(isinstance(h,bool) or not isinstance(h,int) for h in raw_horizons)):
        raise ValueError('Recycler market-window configuration is invalid.')
    blocked=market.get('blocked_horizons_seconds',[])
    if (not isinstance(blocked,list) or
            any(isinstance(h,bool) or not isinstance(h,int) for h in blocked)):
        raise ValueError('Recycler blocked-window configuration is invalid.')
    blocked=set(blocked)
    for flag in ('block_5m','block_15m'):
        if flag in market and not isinstance(market[flag],bool):
            raise ValueError('Recycler blocked-window configuration is invalid.')
    if market.get('block_5m') is True: blocked.add(300)
    if market.get('block_15m') is True: blocked.add(900)
    supported={300,900}
    if maniac: supported.update((3600,14400,86400))
    active=sorted({h for h in raw_horizons if h in supported and h not in blocked})
    if c['recycler_settlement_only']:
        if not active or len(active)>2 or any(h not in (300,900) for h in active):
            raise ValueError('Settlement-only Recycler needs one or both unblocked 5m/15m windows.')
    elif len(active)!=1:
        raise ValueError('Active Recycler needs exactly one supported, unblocked market window.')
    if not c['recycler_settlement_only']:
        horizon=active[0]
        cutoff=c['recycler_entry_cutoff_secs']
        if cutoff>=horizon:
            raise ValueError('Recycler entry cutoff would block the entire configured window.')
        if not maniac and horizon==900 and cutoff<360:
            raise ValueError('15m Recycler profiles must stop entries with at least 360 seconds remaining.')
        if not maniac and horizon==300 and c['recycler_late_maker_secs']>=cutoff:
            raise ValueError('5m Recycler late maker exit must begin after the entry cutoff.')
    if (maniac and c['recycler_settlement_entry_enabled'] and
            c['recycler_entry_cutoff_secs']<=c['recycler_settlement_entry_secs']):
        raise ValueError('Maniac Recycler cycle cutoff must leave a separate late settlement-entry phase.')


def set_value(app, content, section, key, value):
    literal = json.dumps(value)
    updated, found = app._set_config_value(content, section, key, literal)
    if found: return updated
    header = '['+section+']'
    pattern = re.compile(r'^\['+re.escape(section)+r'\][ \t]*(?:#[^\n]*)?$',re.M)
    if pattern.search(content):
        return pattern.sub(lambda m:m.group(0)+'\n'+key+' = '+literal,content,count=1)
    return content+'\n'+header+'\n'+key+' = '+literal+'\n'


def atomic_write(path, content):
    tomllib.loads(content)
    fd, tmp = tempfile.mkstemp(prefix='.fleet-',dir=str(path.parent))
    try:
        with os.fdopen(fd,'w') as stream:
            stream.write(content);stream.flush();os.fsync(stream.fileno())
        os.chmod(tmp,0o600);os.replace(tmp,path)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)


def diagnostics(app, instance):
    path=Path(app.ROOT)/'logs'/f'polypulse-{instance}.log'
    try:
        with path.open('rb') as stream:
            stream.seek(0,2);stream.seek(max(0,stream.tell()-500_000));lines=stream.read().decode(errors='replace').splitlines()
    except OSError:return {'message':'No diagnostic log available','gates':[]}
    heartbeat=next((s for s in reversed(lines) if 'autopilot heartbeat:' in s or 'RECYCLER mode=' in s),'')
    gates=re.findall(r'([a-z_]+)=(\d+)',heartbeat.partition('gates=[')[2])
    evidence=next((s for s in reversed(lines) if any(x in s for x in ('wpred skip','rule veto','ENDGAME veto','DAILY LOSS KILL','RISK BLOCK'))),'')
    return {'message':app._polytron_redact(evidence[-500:]),'heartbeat':app._polytron_redact(heartbeat[-650:]),
            'gates':[{'gate':k,'count':int(v)} for k,v in gates], 'source':'Recent log observations; counts are from the last recorded sweep, not unique opportunities.'}


def fleet_view(app):
    fleet=app.api_v3_fleet()['fleet']
    analytics=app.api_v2_analytics(1)
    perf={r['instance']:r for r in analytics['instances']}
    counts=app.q("""SELECT o.instance,COUNT(DISTINCT o.client_order_id)::int AS entries
        FROM vp.orders o JOIN vp.fills f USING(client_order_id)
        WHERE o.direction='buy' AND f.ts>now()-interval '24 hours' GROUP BY o.instance""")
    counts={r['instance']:r['entries'] for r in counts}
    for row in fleet:
        name=row['instance'];cfg=app.load_config(name)
        row.update(configured_mode=cfg['mode'],execution_mode='maker',recycler_mode=cfg['strategy'].get('recycler_mode','off'),
            supervisor_running=app._supervisor_running(name),autopilot_on_start=cfg['strategy'].get('autopilot_on_start',False),
            performance=perf.get(name,{}),entries_24h=counts.get(name,0),diagnostics=diagnostics(app,name))
        path=Path(app.CONFIG_DIR)/f'polypulse-{name}.toml'
        row['config_hash']=hashlib.sha256(path.read_bytes()).hexdigest()[:12]
        row['paper_balance']=app._config_stake(str(path))
    return {'fleet':fleet,'server_ts':int(time.time()*1000),'performance_window':'Last 24 hours; all recorded modes, matched exits.'}


def _flat(app, instance):
    mode=app.current_mode(instance)
    positions=app.reconstructed_positions(mode,app.accounting_since(instance,mode),instance)
    if positions.get('positions'):raise ValueError('Close or settle this bot’s positions before changing its mode or paper balance.')
    rows=app.q("""SELECT COUNT(*)::int AS n FROM vp.orders WHERE instance=%s
        AND status::text IN ('submitted','acked','partial','uncertain')""",(instance,),one=True)
    if rows and rows['n']:raise ValueError('Cancel and reconcile outstanding orders before changing mode or paper balance.')


def action(app, body, actor):
    name=str(body.get('instance') or '')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,39}',name):raise ValueError('Bot ID must be 1–40 letters, numbers, underscores or dashes.')
    op=body.get('operation');path=Path(app.CONFIG_DIR)/f'polypulse-{name}.toml'
    store=app.platform_store()
    with app._config_write_lock:
        if op=='create':
            if path.exists():raise ValueError('A bot with this ID already exists.')
            base=body.get('template')
            if base not in app.fleet_instances():raise ValueError('Choose an existing bot as the template.')
            execution=body.get('execution_mode','maker')
            if execution!='maker':raise ValueError('Only post-only maker routing is available.')
            content=(Path(app.CONFIG_DIR)/f'polypulse-{base}.toml').read_text()
            content=set_value(app,content,'runtime','mode','paper')
            content=set_value(app,content,'runtime','instance_label',name)
            content=set_value(app,content,'strategy.hot','autopilot_on_start',False)
            for key,value in EXECUTION_DEFAULTS.items():
                inherited=tomllib.loads(content).get('strategy',{}).get('hot',{}).get(key,value)
                content=set_value(app,content,'strategy.hot',key,execution if key=='entry_order_mode' else inherited)
            content=set_value(app,content,'wallet','starting_balance_usd',1000.0)
            shared_wallet=app._admin_wallet_material()
            # Every new ADMIN bot points to the one shared credential location,
            # including before credentials have been entered. Never inherit a
            # template bot's private key or API values into a new bot profile.
            content=app._admin_shared_wallet_config(content,shared_wallet)
            for key,value in tomllib.loads(content).get('data_collection',{}).items():
                if key.endswith('_enabled') and isinstance(value,bool):content=set_value(app,content,'data_collection',key,False)
            # Exclusive creation: a simultaneous request must never overwrite an existing bot.
            fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
            try:
                with os.fdopen(fd,'w') as f:f.write(content);f.flush();os.fsync(f.fileno())
            except Exception:
                path.unlink(missing_ok=True);raise
            result={'ok':True,'instance':name,'message':'Created stopped in PAPER, entries disabled. Review settings before starting.'}
            evidence={'template':base,'mode':'paper','execution_mode':execution}
        else:
            if name not in app.fleet_instances() or not path.exists():raise ValueError('Unknown bot.')
            if op in ('start','stop','restart'):
                if body.get('confirmed') is not True:raise ValueError('Review and confirm the lifecycle action.')
                if op in ('start','restart') and app.current_mode(name)=='live' and body.get('confirm_live') is not True:raise ValueError('LIVE activation requires explicit confirmation.')
                code,result=app.api_v3_fleet_control({'action':op,'instance':name,'confirm_live':body.get('confirm_live')})
                if code>=400 or not result.get('ok'):raise ValueError(result.get('error') or result.get('out') or 'Service operation failed.')
                evidence={'operation':op,'mode':app.current_mode(name)}
            elif op=='supervisor':
                enabled=body.get('enabled')
                if not isinstance(enabled,bool):raise ValueError('Supervisor enabled must be boolean.')
                if enabled and body.get('confirmed') is not True:raise ValueError('Supervisor can modify strategy settings; confirm enabling it.')
                if enabled and app.current_mode(name)=='live' and body.get('confirm_live') is not True:raise ValueError('LIVE supervision requires explicit confirmation.')
                code,result=app.api_supervisor_control('start' if enabled else 'stop',name)
                if code>=400 or not result.get('ok'):raise ValueError('Supervisor service operation failed.')
                evidence={'enabled':enabled}
            elif op=='properties':
                changes=body.get('changes',{})
                if not isinstance(changes,dict) or not changes or set(changes)-{'mode','paper_balance'}:raise ValueError('Supported properties: mode and paper_balance.')
                if app.instance_running(name):raise ValueError('Stop the bot before changing its mode or starting paper balance.')
                if app._supervisor_running(name):raise ValueError('Stop its supervisor before changing runtime properties.')
                if body.get('confirmed') is not True:raise ValueError('Confirm the new session properties.')
                if 'mode' in changes and changes['mode'] not in ('paper','live'):raise ValueError('Only PAPER and LIVE are supported here.')
                if changes.get('mode')=='live' and body.get('confirm_live') is not True:raise ValueError('Changing to LIVE requires explicit confirmation.')
                _flat(app,name)
                content=path.read_text();old=tomllib.loads(content)
                validate_recycler(old.get('strategy',{}).get('hot',{}),
                    changes.get('mode',old.get('runtime',{}).get('mode','paper')),
                    old.get('polymarket',{}))
                if body.get('expected_hash')!=hashlib.sha256(content.encode()).hexdigest()[:12]:raise ValueError('Configuration changed. Refresh and review again.')
                evidence={'old':{},'new':changes}
                if 'paper_balance' in changes:
                    balance=changes['paper_balance']
                    if isinstance(balance,bool) or not isinstance(balance,(float,int)) or not math.isfinite(balance) or not 10<=balance<=1_000_000:raise ValueError('Paper starting balance must be $10–$1,000,000.')
                    if changes.get('mode',app.current_mode(name))!='paper':raise ValueError('Paper balance is only editable in PAPER mode.')
                    evidence['old']['paper_balance']=old.get('wallet',{}).get('starting_balance_usd')
                    content=set_value(app,content,'wallet','starting_balance_usd',balance)
                if 'mode' in changes:
                    evidence['old']['mode']=old.get('runtime',{}).get('mode')
                    content=set_value(app,content,'runtime','mode',changes['mode'])
                atomic_write(path,content)
                result={'ok':True,'instance':name,'message':'Properties saved. Bot remains stopped; Start is a separate action.'}
            else:raise ValueError('Unknown fleet operation.')
        app._bot_status_cache[0]=0.0;app._since_cache.clear();app._instance_running_cache.pop(name,None)
        store.record(actor,'fleet.'+op,name,evidence)
        return result
