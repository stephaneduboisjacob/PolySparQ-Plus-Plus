"""Versioned operational read models and durable cockpit workflows.

Trading truth stays in PostgreSQL. SQLite holds cockpit incidents, research
results and hash-chained operator audit events; it never places venue orders.
"""
from collections import defaultdict
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import base64
import json
import math
import os
from pathlib import Path
import shutil
import sqlite3
import time
import uuid


def now_ms():
    return int(time.time()*1000)


class PlatformStore:
    def __init__(self, root):
        self.path = Path(root)/'logs'/'cockpit-platform.sqlite3'
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as db:
            db.executescript('''
              CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, actor TEXT NOT NULL,
                type TEXT NOT NULL, instance TEXT, payload TEXT NOT NULL,
                previous_hash TEXT NOT NULL, hash TEXT NOT NULL);
              CREATE TABLE IF NOT EXISTS incidents (
                id TEXT PRIMARY KEY, fingerprint TEXT UNIQUE, severity TEXT,
                title TEXT, instance TEXT, state TEXT, first_seen INTEGER,
                last_seen INTEGER, occurrences INTEGER, evidence TEXT,
                acknowledged_by TEXT, resolved_at INTEGER);
              CREATE TABLE IF NOT EXISTS config_versions (
                hash TEXT PRIMARY KEY, instance TEXT, ts INTEGER, actor TEXT,
                reason TEXT, settings TEXT NOT NULL);
              CREATE TABLE IF NOT EXISTS research_runs (
                id TEXT PRIMARY KEY, ts INTEGER, actor TEXT, instance TEXT,
                kind TEXT, inputs TEXT, result TEXT);
            ''')
        os.chmod(self.path, 0o600)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        db.execute('PRAGMA journal_mode=WAL')
        try:
            with db:
                yield db
        finally:
            db.close()

    @staticmethod
    def _event(db, actor, kind, instance, payload):
        previous = db.execute('SELECT hash FROM events ORDER BY id DESC LIMIT 1').fetchone()
        previous_hash = previous['hash'] if previous else '0'*64
        ts = now_ms()
        text = json.dumps(payload, sort_keys=True, separators=(',', ':'), allow_nan=False)
        canonical = json.dumps([ts, actor, kind, instance, text, previous_hash], separators=(',', ':'))
        digest = hashlib.sha256(canonical.encode()).hexdigest()
        db.execute('INSERT INTO events(ts,actor,type,instance,payload,previous_hash,hash) VALUES(?,?,?,?,?,?,?)',
                   (ts, actor, kind, instance, text, previous_hash, digest))
        return digest

    def record(self, actor, kind, instance, payload):
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            return self._event(db, actor, kind, instance, payload)

    def events(self, limit=100):
        with self.connect() as db:
            rows = db.execute('SELECT * FROM events ORDER BY id DESC LIMIT ?', (min(500,limit),)).fetchall()
        return [{**dict(r), 'payload':json.loads(r['payload'])} for r in rows]

    def config(self, instance, settings, actor='observer', reason='observed configuration'):
        text = json.dumps(settings, sort_keys=True, separators=(',', ':'), allow_nan=False)
        digest = hashlib.sha256((instance+'\n'+text).encode()).hexdigest()
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            inserted = db.execute('INSERT OR IGNORE INTO config_versions VALUES(?,?,?,?,?,?)',
                (digest, instance, now_ms(), actor, reason, text)).rowcount
            if inserted:
                self._event(db, actor, 'config.observed', instance, {'config_hash':digest,'reason':reason})
        return digest

    def configs(self, instance):
        with self.connect() as db:
            rows = db.execute('SELECT * FROM config_versions WHERE instance=? ORDER BY ts DESC LIMIT 100',
                              (instance,)).fetchall()
        return [{**dict(r), 'settings':json.loads(r['settings'])} for r in rows]

    def observe_incidents(self, observations):
        stamp = now_ms()
        fingerprints = set()
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            for item in observations:
                fingerprint = item['fingerprint']
                fingerprints.add(fingerprint)
                old = db.execute('SELECT * FROM incidents WHERE fingerprint=?', (fingerprint,)).fetchone()
                evidence = json.dumps(item.get('evidence',{}), sort_keys=True)
                if not old:
                    incident_id = str(uuid.uuid4())
                    db.execute('INSERT INTO incidents VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',
                      (incident_id, fingerprint, item['severity'], item['title'], item.get('instance'),
                       'OPEN', stamp, stamp, 1, evidence, None, None))
                    self._event(db,'system','incident.opened',item.get('instance'),{'incident_id':incident_id,'title':item['title']})
                else:
                    state = 'OPEN' if old['state']=='RESOLVED' else old['state']
                    db.execute('UPDATE incidents SET severity=?,title=?,last_seen=?,occurrences=occurrences+1,evidence=?,state=?,resolved_at=NULL WHERE id=?',
                               (item['severity'],item['title'],stamp,evidence,state,old['id']))
                    if old['state']=='RESOLVED':
                        self._event(db,'system','incident.reopened',item.get('instance'),{'incident_id':old['id']})
            # Recovery follows a successful complete observation only.
            for old in db.execute("SELECT * FROM incidents WHERE state!='RESOLVED'").fetchall():
                if old['fingerprint'] not in fingerprints:
                    db.execute("UPDATE incidents SET state='RESOLVED',resolved_at=? WHERE id=?", (stamp,old['id']))
                    self._event(db,'system','incident.resolved',old['instance'],{'incident_id':old['id']})
        return self.incidents()

    def incidents(self):
        with self.connect() as db:
            rows = db.execute("SELECT * FROM incidents ORDER BY (state='RESOLVED'), CASE severity WHEN 'CRITICAL' THEN 0 WHEN 'HIGH' THEN 1 ELSE 2 END, last_seen DESC LIMIT 200").fetchall()
        return [{**dict(r),'evidence':json.loads(r['evidence'])} for r in rows]

    def acknowledge(self, incident_id, actor):
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT * FROM incidents WHERE id=?',(incident_id,)).fetchone()
            if not row:
                raise ValueError('Incident not found')
            if row['state']=='RESOLVED':
                raise ValueError('Incident is already resolved')
            db.execute("UPDATE incidents SET state='ACKNOWLEDGED',acknowledged_by=? WHERE id=?",(actor,incident_id))
            self._event(db,actor,'incident.acknowledged',row['instance'],{'incident_id':incident_id})

    def research(self, actor, instance, kind, inputs, result):
        run_id = str(uuid.uuid4())
        with self.connect() as db:
            db.execute('BEGIN IMMEDIATE')
            db.execute('INSERT INTO research_runs VALUES(?,?,?,?,?,?,?)',
               (run_id,now_ms(),actor,instance,kind,json.dumps(inputs),json.dumps(result,allow_nan=False)))
            self._event(db,actor,'research.completed',instance,{'run_id':run_id,'kind':kind})
        return run_id

    def runs(self, instance=''):
        with self.connect() as db:
            rows=db.execute('SELECT * FROM research_runs WHERE (?=\'\' OR instance=?) ORDER BY ts DESC LIMIT 100',
                            (instance,instance)).fetchall()
        return [{**dict(r),'inputs':json.loads(r['inputs']),'result':json.loads(r['result'])} for r in rows]


def risk_view(app):
    rows=[]
    assets=defaultdict(float)
    for instance in app.fleet_instances():
        config=app.load_config(instance)
        strategy=config.get('strategy',{})
        mode=app.current_mode(instance)
        snapshot=app.reconstructed_positions(mode,app.accounting_since(instance,mode),instance)
        positions=snapshot.get('positions',[])
        deployed=sum(float(p.get('cost_basis') or 0) for p in positions)
        cap=float(strategy.get('max_deployed_usd') or 0)
        for p in positions:
            assets[p.get('asset') or 'unknown']+=float(p.get('cost_basis') or 0)
        rows.append({'instance':instance,'mode':mode.upper(),'deployed':deployed,
                     'configured_cap':cap,'budget_used_pct':100*deployed/cap if cap>0 else None,
                     'positions':len(positions),'max_order_usd':strategy.get('max_order_usd'),
                     'kill_pct':strategy.get('daily_loss_kill_pct',config.get('risk',{}).get('daily_loss_kill_pct'))})
    return {'instances':rows,'assets':[{'asset':a,'deployed':v} for a,v in assets.items()],
            'raw_exposure':sum(r['deployed'] for r in rows),'server_ts':now_ms(),
            'note':'Configured instance caps. Correlated exposure and global reservation enforcement are not yet available.'}


def system_view(app):
    usage=shutil.disk_usage(app.ROOT)
    mem={}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key,value=line.split(':',1)
        if key in ('MemTotal','MemAvailable'): mem[key]=int(value.split()[0])*1024
    started=time.monotonic()
    app.q('SELECT 1 AS healthy',one=True)
    return {'server_ts':now_ms(),'load_average':list(os.getloadavg()),'cpu_count':os.cpu_count(),
            'memory_total':mem.get('MemTotal'),'memory_available':mem.get('MemAvailable'),
            'disk_total':usage.total,'disk_free':usage.free,'disk_used_pct':100*usage.used/usage.total,
            'database_latency_ms':round((time.monotonic()-started)*1000,2),
            'database':'CONNECTED','disk_growth_per_day':None,'storage_forecast':None}


def signals_view(app, instance='', search='', limit=100):
    rows=app.q('''SELECT id,ts,condition_id,side_label::text,direction::text,confidence,
                recommended,rec_size_usd,rec_limit_price,reason,model_version,regime_label,
                payload->>'instance' AS instance,payload->>'client_order_id' AS order_id
         FROM vp.signals WHERE (%s='' OR payload->>'instance'=%s)
           AND (%s='' OR concat_ws(' ',condition_id,reason,recommended,model_version) ILIKE %s)
         ORDER BY ts DESC,id DESC LIMIT %s''',
         (instance,instance,search,'%'+search+'%',max(1,min(500,limit))))
    for row in rows: row['reason']=app._polytron_redact(row.get('reason') or '')
    return {'signals':rows,'server_ts':now_ms(),'gate_detail':'Recorded strategy reasons; full gate evaluation snapshots are not yet emitted by all strategies.'}


def transactions_view(app, qs):
    """All historical fills, including previous paper sessions and retired bots."""
    instance=qs.get('instance',[''])[0]
    days=max(0,min(3650,int(qs.get('days',['30'])[0])))
    limit=max(1,min(250,int(qs.get('limit',['100'])[0])))
    mode=qs.get('mode',[''])[0].lower()
    if mode not in ('','paper','live','replay','shadow'): raise ValueError('Invalid mode')
    where=['(%s=\'\' OR COALESCE(o.instance,\'\')=%s)', '(%s=\'\' OR o.mode::text=%s)']
    args=[instance,instance,mode,mode]
    if days:
        where.append("f.ts >= now() - (%s * interval '1 day')");args.append(days)
    for name,operator in [('from','>='),('to','<')]:
        if qs.get(name,[''])[0]:
            stamp=int(qs[name][0])
            if stamp<0 or stamp>now_ms()+86_400_000: raise ValueError('Invalid time range')
            where.append(f'f.ts {operator} to_timestamp(%s/1000.0)');args.append(stamp)
    search=qs.get('search',[''])[0][:200]
    if search:
        where.append("concat_ws(' ',o.condition_id,o.client_order_id::text,o.source,o.instance,m.asset) ILIKE %s")
        args.append('%'+search+'%')
    filters=' AND '.join(where)
    totals=app.q('''SELECT COUNT(*)::int AS fills,
          COALESCE(SUM(f.price*f.size_shares) FILTER(WHERE o.direction='buy'),0) AS purchased,
          COALESCE(SUM(f.price*f.size_shares) FILTER(WHERE o.direction='sell'),0) AS proceeds,
          COALESCE(SUM(f.fee_usd),0) AS fees
        FROM vp.fills f JOIN vp.orders o USING(client_order_id)
        LEFT JOIN vp.pm_markets m USING(condition_id) WHERE '''+filters,tuple(args),one=True)
    cursor=qs.get('cursor',[''])[0]
    if cursor:
        try:
            if len(cursor)>2048: raise ValueError('Cursor too long')
            decoded=json.loads(base64.urlsafe_b64decode(cursor.encode()).decode())
            if not isinstance(decoded,dict): raise ValueError('Invalid cursor')
            stamp=datetime.fromisoformat(decoded['ts'])
            if stamp.tzinfo is None: raise ValueError('Cursor timestamp requires timezone')
            ident=int(decoded['id'])
        except (ValueError,KeyError,TypeError): raise ValueError('Invalid history cursor')
        where.append('(f.ts,f.id)<(%s::timestamptz,%s::bigint)');args.extend([stamp,ident])
    rows=app.q('''SELECT f.id,f.ts,f.ts::text AS cursor_ts,f.price,f.size_shares,f.fee_usd,
            o.client_order_id::text,o.instance,o.mode::text,o.session_id::text,
            o.condition_id,o.side_label::text,o.direction::text,o.source,o.execution_mode,
            m.asset,m.horizon_seconds,
            (CASE WHEN o.direction='sell' THEN 1 ELSE -1 END)*f.price*f.size_shares-f.fee_usd AS cash_flow
        FROM vp.fills f JOIN vp.orders o USING(client_order_id)
        LEFT JOIN vp.pm_markets m USING(condition_id) WHERE '''+' AND '.join(where)+
        ' ORDER BY f.ts DESC,f.id DESC LIMIT %s',tuple(args+[limit+1]))
    more=len(rows)>limit;rows=rows[:limit]
    next_cursor=None
    if more and rows:
        next_cursor=base64.urlsafe_b64encode(json.dumps({'ts':rows[-1]['cursor_ts'],'id':rows[-1]['id']}).encode()).decode()
    for r in rows:r.pop('cursor_ts',None)
    return {'transactions':rows,'totals':totals,'next_cursor':next_cursor,'server_ts':now_ms(),
            'instance':instance or 'ALL BOTS','days':days,'scope':'All recorded sessions and modes unless explicitly filtered.'}


def replay_view(app, timestamp, instance=''):
    if not 0 < timestamp <= now_ms(): raise ValueError('Choose a past timestamp')
    snapshots=app.q('''SELECT DISTINCT ON(instance) instance,mode::text,ts,equity_usd,
           deployed_usd,realized_pnl,unrealized_pnl,open_positions
        FROM vp.positions_history WHERE ts<=to_timestamp(%s/1000.0)
          AND (%s='' OR instance=%s) ORDER BY instance,ts DESC''',(timestamp,instance,instance))
    orders=app.q('''SELECT client_order_id::text,instance,mode::text,ts_created,
         condition_id,side_label::text,direction::text,execution_mode,price,size_shares
       FROM vp.orders WHERE ts_created<=to_timestamp(%s/1000.0)
         AND ts_created>to_timestamp(%s/1000.0)-interval '30 minutes'
         AND (%s='' OR instance=%s) ORDER BY ts_created DESC LIMIT 100''',
        (timestamp,timestamp,instance,instance))
    return {'as_of':timestamp,'snapshots':snapshots,'orders':orders,'server_ts':now_ms(),
            'method':'Last stored position snapshot per instance, plus order creation records. Current order status is deliberately omitted because it may have changed after the selected time.'}


def run_research(app, store, actor, body):
    kind=body.get('kind')
    instance=str(body.get('instance') or '')
    if instance and instance not in app.fleet_instances(): raise ValueError('Unknown instance')
    if kind=='execution_cost':
        extra=float(body.get('extra_cost_cents',0))
        scale=float(body.get('size_multiplier',1))
        if not math.isfinite(extra) or not -10<=extra<=20: raise ValueError('Extra cost must be -10 to 20 cents/share')
        if not math.isfinite(scale) or not 0.1<=scale<=5: raise ValueError('Size multiplier must be 0.1 to 5')
        data=app.api_v2_analytics(max(1,min(90,int(body.get('days',14)))))
        exits=[e for e in data['exits'] if not instance or e.get('instance')==instance]
        baseline=sum(e['realized_pnl'] for e in exits)
        scenario=[(e['realized_pnl']-extra/100*e['shares'])*scale for e in exits]
        result={'simulated':True,'sample_size':len(exits),'baseline_net':baseline,
                'proposed_net':sum(scenario),'delta':sum(scenario)-baseline,
                'assumptions':['Same recorded exits and fill quantities, scaled linearly.',
                 'Additional cost applies per exited share. No claim about queue fills or available capacity.',
                 'This is execution-cost sensitivity, not a replay of changed strategy gates.']}
        inputs={'days':data['days_window'],'extra_cost_cents':extra,'size_multiplier':scale}
    elif kind=='all_positions_lose':
        rows=app.api_v3_overview()['positions']
        rows=[p for p in rows if not instance or p['instance']==instance]
        loss=sum(p['size'] for p in rows)
        result={'simulated':True,'sample_size':len(rows),'loss_from_cost_basis':loss,
                'assumptions':['Every currently open binary claim settles at zero.',
                               'Resting orders and positions not represented in the ledger are excluded.']}
        inputs={'scenario':kind}
    else:
        raise ValueError('Supported research: execution_cost, all_positions_lose')
    run_id=store.research(actor,instance,kind,inputs,result)
    return {'ok':True,'run_id':run_id,**result}
