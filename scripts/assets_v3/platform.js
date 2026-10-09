/* PolySparQ++ operational views. All trading/accounting calculations are server owned. */
(() => {
  const money = v => v == null ? '—' : '$' + Number(v).toFixed(2);
  const num = v => v == null ? '—' : Number(v).toLocaleString(undefined,{maximumFractionDigits:3});
  const when = v => v ? new Date(Number(v)).toLocaleString() : '—';
  const note = text => `<p class="warn-panel">${esc(text)}</p>`;
  const table = (rows, columns) => `<div style="overflow:auto"><table><thead><tr>${columns.map(c=>`<th>${esc(c[0])}</th>`).join('')}</tr></thead><tbody>${rows.map(r=>`<tr>${columns.map(c=>`<td>${c[2]?c[2](r[c[1]],r):esc(r[c[1]]??'—')}</td>`).join('')}</tr>`).join('')||`<tr><td colspan="${columns.length}" class="empty">No records in this scope.</td></tr>`}</tbody></table></div>`;
  const kpis = values => `<div class="grid g4" style="margin:12px 0">${values.map(([k,v])=>`<div class="card kpi"><small>${esc(k)}</small><div class="v">${esc(v)}</div></div>`).join('')}</div>`;
  const views = {
    opportunities:'Opportunities', orders:'Orders', decisions:'Decision Traces', risk:'Risk',
    execution:'Execution', research:'Research Lab', simulator:'Simulator', replay:'Replay',
    incidents:'Incidents', configuration:'Configuration History', audit:'Audit', system:'System'
  };
  const art = {
    opportunities:['opportunity-radar.webp','Opportunity radar'], orders:['orders-hero.webp','Order lifecycle'],
    decisions:['decision-trace-hero.webp','Decision trace'], risk:['risk-center-hero.webp','Risk controls'],
    execution:['execution-hero.webp','Execution quality'], research:['research-hero.webp','Research lab'],
    simulator:['sim-sandbox.webp','Simulation sandbox'], replay:['replay-hero.webp','Replay timeline'],
    incidents:['incidents-hero.webp','Incident response'], configuration:['config-history-hero.webp','Configuration history'],
    audit:['audit-hero.webp','Audit ledger'], system:['system-health-hero.webp','System health']
  };
  const descriptions = {
    opportunities:'Ranked prediction-board observations. Edge estimates are analytical, not order instructions.',
    orders:'Inspect order attempts, execution mode, state, and recorded rejection reasons.',
    decisions:'Search persisted strategy decisions and the gate details the engine recorded.',
    risk:'Review observed exposure and configured limits before changing fleet behavior.',
    execution:'Observed latency and maker order state. Queue estimates are not fabricated.',
    research:'Saved experiments and evidence from the available replay and sensitivity tools.',
    simulator:'Compare recorded outcomes under clearly labeled execution-cost assumptions.',
    replay:'Read-only reconstruction from persisted position snapshots and order records.',
    incidents:'Grouped operational symptoms, acknowledgements, and recovery guidance.',
    configuration:'Inspect prior configuration versions and review a restore diff before applying.',
    audit:'Review operator actions and the recorded hash-linked audit sequence.',
    system:'Current host and database telemetry observed by the cockpit service.'
  };
  for (const [id,title] of Object.entries(views)) {
    TITLES[id]=title;
    const section=document.createElement('section');section.id='v-'+id;section.className='view';
    const [image,alt]=art[id];
    section.innerHTML=`<div class="v4-hero"><img class="v4-hero-art" src="/assets_v4/${image}?v=5.1" alt="${alt} artwork"><div class="v4-hero-copy"><div class="v4-kicker">PolySparQ++ · ${title}</div><div class="v4-title">${title}</div><div class="v4-desc">${descriptions[id]}</div></div><img class="v4-hero-full-logo" src="/assets_v4/polysparq_logo_full_with_name.webp?v=5.3" alt="PolySparQ++"></div><div class="v4-hero-toolbar"><button data-refresh="${id}">Refresh</button></div><p class="sub" id="${id}-fresh">Loading…</p><div id="${id}-body" aria-live="polite"></div>`;
    document.querySelector('main').append(section);
    section.querySelector('[data-refresh]').onclick=()=>load(id);
  }
  const nav=document.querySelector('nav');
  const navIcons={opportunities:'◎',orders:'↔',decisions:'⌘',risk:'△',execution:'⇄',research:'⌕',simulator:'◈',replay:'↶',incidents:'!',configuration:'≡',audit:'≣',system:'◌'};
  const menu=[['Trading',[['overview','Command Center'],['opportunities','Opportunities'],['markets','Markets'],['positions','Positions'],['orders','Orders'],['maker','Maker Fleet'],['fleet','Instances']]],
    ['Strategy & risk',[['lab','Strategies'],['risk','Risk'],['execution','Execution'],['analytics','Analytics'],['decisions','Decision Traces']]],
    ['Research',[['research','Research Lab'],['simulator','Simulator'],['replay','Replay'],['board','Prediction Board']]],
    ['Operations',[['feeds','Data / Feeds'],['account','Wallets'],['incidents','Incidents'],['editor','Configuration'],['configuration','Config History'],['audit','Audit'],['system','System'],['logs','Logs']]]];
  const side=$('sideCard');
  nav.innerHTML=menu.map(([group,items])=>`<div class="grp">${group}</div>${items.map(([id,label])=>`<a href="#${id}" data-v="${id}"><span class="ic" aria-hidden="true">${navIcons[id]||'◇'}</span>${label}</a>`).join('')}`).join('')+'<a href="#polytron" id="polytron-nav"><span class="ic">◉</span>PolyTron</a>';
  nav.append(side);
  const alertBadge=document.createElement('span');alertBadge.id='alertN';alertBadge.style.marginLeft='auto';nav.querySelector('[data-v=incidents]').append(alertBadge);
  $('polytron-nav').onclick=e=>{e.preventDefault();ptToggle(true)};
  const oldRoute=route;
  removeEventListener('hashchange',oldRoute);
  route=function(){oldRoute();const id=location.hash.slice(1);if(views[id])load(id)};
  addEventListener('hashchange',route);
  const pending=new Set(), sequences={};
  const scoped = path => path+'?instance='+encodeURIComponent(S.inst);
  let latestAnalytics=null;
  const loaders={
    async orders(){
      const data=await jf(scoped('/api/v4/orders'));
      return note('Scope: '+(S.inst||'ALL BOTS')+'. Latest 200 order attempts across all sessions and modes. Complete paginated fills are in Positions → Transaction history.')+
        '<input id="order-search" aria-label="Search orders" placeholder="Filter by market, status, side, or source" style="width:100%;margin:10px 0">'+
        table(data.orders||[],[['Time','ts_created',when],['Bot','instance'],['Mode','mode'],['Asset','asset'],['Side','side_label'],['Action','direction'],['Execution','execution_mode'],['Status','status'],['Price','price',num],['Shares','size_shares',num],['Fees','fee_usd',money],['Source','source'],['Reason','reject_reason']]);
    },
    async decisions(){
      const data=await jf(scoped('/api/v4/signals')+'&search='+encodeURIComponent(S.traceSearch||''));
      return `<form id="trace-search"><input aria-label="Search decision traces" name="search" placeholder="Market ID or rejection reason" value="${esc(S.traceSearch||'')}" style="width:65%"><button>Search</button></form>`+
        note(data.gate_detail)+table(data.signals,[['Time','ts',when],['Instance','instance'],['Decision','recommended'],['Side','side_label'],['Confidence','confidence',num],['Size','rec_size_usd',money],['Reason','reason'],['Model','model_version'],['Order ID','order_id']]);
    },
    async opportunities(){
      const data=await jf('/api/manual_predictions');
      const cards=(data.cards||[]).filter(c=>c.period==='CURRENT').sort((a,b)=>(b.conservative_edge??-1)-(a.conservative_edge??-1));
      return note('Experimental prediction-board opportunities, ranked by its conservative net edge. These are analytical observations; bot strategy gates may differ. Snapshot age: '+num(data.snapshot_age_s)+' seconds.')+
        table(cards,[['Asset','asset'],['Horizon','horizon_seconds',v=>esc(v/60+'m')],['Side','direction'],['Model state','model_status'],['Confidence','chance',v=>esc(v==null?'—':(100*v).toFixed(1)+'%')],['Net edge ¢','model_edge',v=>esc(v==null?'—':(100*v).toFixed(2))],['Conservative edge ¢','conservative_edge',v=>esc(v==null?'—':(100*v).toFixed(2))],['Book age s','book_age_s',num],['Seconds left','seconds_left',num]]);
    },
    async risk(){
      const data=await jf('/api/v4/risk');
      return kpis([['Raw deployed exposure',money(data.raw_exposure)],['Instances',data.instances.length],['Assets with exposure',data.assets.length],['Scope','Fleet']])+note(data.note)+
        table(data.instances,[['Instance','instance'],['Mode','mode'],['Positions','positions'],['Deployed','deployed',money],['Configured cap','configured_cap',money],['Budget used %','budget_used_pct',num],['Max order','max_order_usd',money]])+
        '<h2 class="sec">Exposure by asset</h2>'+table(data.assets,[['Asset','asset'],['Exposure','deployed',money]])+
        '<button id="stress-loss" style="margin-top:12px">Simulate all open positions losing</button>';
    },
    async execution(){
      const [latency,maker]=await Promise.all([jf('/api/v3/latency'),jf('/api/v4/maker_fleet')]);
      return note('Latency is persisted submission-to-first-fill duration in seconds. Queue position and fill predictions require engine observations; no values are inferred.')+
        table(latency.latency||[],[['Instance','instance'],['p50 seconds','p50',num],['p90 seconds','p90',num],['Filled order %','fill_rate',num],['Maker/hybrid order %','maker_pct',num]])+
        kpis([['Resting orders',maker.summary.resting_orders],['Resting notional',money(maker.summary.resting_notional)],['Uncertain orders',maker.summary.uncertain_orders],['Stale quotes',maker.summary.stale_quotes]])+
        '<button id="cancel-quotes">Preview cancellation for selected instance</button>';
    },
    async incidents(){
      const data=await jf('/api/v4/incidents');
      return note('Related observations are deduplicated. Acknowledgement records operator attention; automatic resolution requires the condition to recover.')+
        table(data.incidents,[['Severity','severity'],['State','state'],['Instance','instance'],['Incident','title'],['First seen','first_seen',when],['Last seen','last_seen',when],['Observations','occurrences'],['Action','id',(id,r)=>r.state==='OPEN'?`<button data-ack="${esc(id)}">Acknowledge</button>`:esc(r.acknowledged_by||r.state)]])+
        '<h2 class="sec">Runbook</h2><div class="card">For an uncertain order: pause the affected instance, inspect venue orders and fills, and reconcile its ledger before resuming. For missing runtime telemetry: check the process and its logs; an older binary may not emit heartbeats. For stale feeds: inspect the source connection and last event timestamp before rearming entries.</div>';
    },
    async system(){
      const data=await jf('/api/v4/system');
      return kpis([['CPU cores',data.cpu_count],['Load 1 / 5 / 15m',data.load_average.map(x=>x.toFixed(2)).join(' / ')],['Database round trip',data.database_latency_ms+' ms'],['Disk used',data.disk_used_pct.toFixed(1)+'%']])+
        table([{name:'Available memory',value:num(data.memory_available/2**30)+' GiB'},{name:'Total memory',value:num(data.memory_total/2**30)+' GiB'},{name:'Free disk',value:num(data.disk_free/2**30)+' GiB'},{name:'Database',value:data.database}], [['Resource','name'],['Observed value','value']]);
    },
    async audit(){
      const data=await jf('/api/v4/audit');
      return note('Cockpit operational events are hash chained in append order. This detects modification when checked against a trusted hash; it is not an external audit anchor.')+
        table(data.events,[['Time','ts',when],['Actor','actor'],['Event','type'],['Instance','instance'],['Evidence','payload',v=>`<details><summary>Inspect</summary><pre style="white-space:pre-wrap">${esc(JSON.stringify(v,null,2))}</pre></details>`],['Hash','hash',v=>`<code title="${esc(v)}">${esc(v.slice(0,12))}</code>`]]);
    },
    async configuration(){
      if(!selectedConfig())return note('Choose one current bot in the top bar to view or restore configuration versions.');
      const scope=S.inst;const data=await jf(scoped('/api/v4/config_versions'));if(scope===S.inst)S.versions=data.versions;
      return note('Versioned trading controls only; credentials and infrastructure secrets are excluded. Restore opens a diff in the editor for explicit Apply.')+
        table(data.versions,[['Time','ts',when],['Version','hash',v=>esc(v.slice(0,12))],['Actor','actor'],['Reason','reason'],['Action','hash',v=>`<button data-restore="${esc(v)}">Review restore</button>`]]);
    },
    async research(){
      const data=await jf(scoped('/api/v4/research'));
      return '<p class="sub">Saved simulated experiments for '+esc(S.inst)+'. Run an experiment in Simulator or a stress scenario in Risk.</p>'+table(data.runs,[['Time','ts',when],['Run','id'],['Kind','kind'],['Inputs','inputs',v=>esc(JSON.stringify(v))],['Result','result',v=>`<details><summary>Evidence and assumptions</summary><pre style="white-space:pre-wrap">${esc(JSON.stringify(v,null,2))}</pre></details>`]]);
    },
    async simulator(){
      return `<img src="/assets_v4/sim-sandbox.webp?v=5.1" alt="Simulation scenario artwork" style="width:100%;max-height:128px;object-fit:cover;border-radius:10px"><h2 class="sec">Execution cost and size sensitivity</h2>${note('Uses matched historical exits with their recorded fees. This simulation holds entry and exit decisions fixed; it does not model queue priority, changed strategy gates, or available market capacity.')}<form id="research-form" class="card"><label>Lookback days <input name="days" type="number" min="1" max="90" value="14"></label> <label>Extra cost ¢/share <input name="extra_cost_cents" type="number" min="-10" max="20" step="0.1" value="0.5"></label> <label>Size multiplier <input name="size_multiplier" type="number" min="0.1" max="5" step="0.1" value="1"></label><button class="primary">Run and save simulation</button></form><div id="research-result"></div>`;
    },
    async replay(){
      const ts=S.replayAt||Date.now()-60_000;
      const data=await jf(scoped('/api/v4/replay')+'&ts='+ts);
      return `<form id="replay-form"><label>UTC <input name="at" type="datetime-local" value="${new Date(ts).toISOString().slice(0,16)}" required></label><button>Load historical state</button></form>`+note(data.method)+
        '<h2 class="sec">Recorded position snapshots</h2>'+table(data.snapshots,[['Instance','instance'],['Mode','mode'],['Snapshot time','ts',when],['Equity','equity_usd',money],['Deployed','deployed_usd',money],['Realized','realized_pnl',money]])+
        '<h2 class="sec">Orders created in the preceding 30 minutes</h2>'+table(data.orders,[['Time','ts_created',when],['Instance','instance'],['Mode','mode'],['Side','side_label'],['Direction','direction'],['Execution','execution_mode'],['Price','price',num],['Shares','size_shares',num]]);
    }
  };
  async function action(body){return jf('/api/v4/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})}
  async function load(id){
    if(pending.has(id))return;
    pending.add(id);const instance=S.inst,seq=(sequences[id]||0)+1;sequences[id]=seq;
    const el=$(id+'-body'),stamp=$(id+'-fresh');
    try{
      const html=await loaders[id]();
      if(S.inst!==instance){setTimeout(()=>load(id),0);return}
      el.innerHTML=html;stamp.textContent='Last updated '+new Date().toLocaleTimeString()+' · '+(['risk','execution','system','incidents','audit','opportunities'].includes(id)?'Fleet / shared platform':(instance||'ALL BOTS'));stamp.dataset.ts=Date.now();
      wire(id,el);
    }catch(e){el.innerHTML=`<div class="warn-panel" role="alert">Unable to load ${esc(views[id])}: ${esc(e.message)}</div>`;stamp.textContent='UNAVAILABLE · last successful data is not current'}
    finally{pending.delete(id)}
  }
  function wire(id,el){
    el.querySelectorAll('[data-ack]').forEach(b=>b.onclick=async()=>{try{await action({action:'acknowledge_incident',incident_id:b.dataset.ack});await load(id)}catch(e){toast(e.message,'err')}});
    el.querySelectorAll('[data-restore]').forEach(b=>b.onclick=async()=>{
      const scope=S.inst;const version=S.versions.find(v=>v.hash===b.dataset.restore);
      const current=await jf(scoped('/api/v3/knobs'));if(scope!==S.inst)return;S.knobs=current;S.dirty={};
      current.groups.forEach(g=>g.rows.forEach(r=>{if(r.active&&Object.hasOwn(version.settings,r.key)&&JSON.stringify(r.value)!==JSON.stringify(version.settings[r.key]))S.dirty[r.key]=version.settings[r.key]}));
      location.hash='#editor';renderEditorDynamic();toast('Restore diff ready for review');
    });
    if(id==='orders')$('order-search').oninput=e=>el.querySelectorAll('tbody tr').forEach(r=>r.hidden=!r.textContent.toLowerCase().includes(e.target.value.toLowerCase()));
    if(id==='decisions')$('trace-search').onsubmit=e=>{e.preventDefault();S.traceSearch=new FormData(e.target).get('search');load(id)};
    if(id==='replay')$('replay-form').onsubmit=e=>{e.preventDefault();S.replayAt=Date.parse(new FormData(e.target).get('at')+'Z');load(id)};
    if(id==='simulator')$('research-form').onsubmit=async e=>{
      e.preventDefault();const values=Object.fromEntries(new FormData(e.target));const button=e.target.querySelector('button');button.disabled=true;
      try{const result=await action({action:'research',kind:'execution_cost',instance:S.inst,...values});$('research-result').innerHTML=kpis([['SIMULATED baseline',money(result.baseline_net)],['SIMULATED proposal',money(result.proposed_net)],['Difference',money(result.delta)],['Matched exits',result.sample_size]])+note(result.assumptions.join(' '))}catch(err){toast(err.message,'err')}finally{button.disabled=false}
    };
    if(id==='risk')$('stress-loss').onclick=async()=>{try{const r=await action({action:'research',kind:'all_positions_lose',instance:''});toast('SIMULATED loss from cost basis: '+money(r.loss_from_cost_basis)+' · '+r.sample_size+' positions. Saved in Research Lab.')}catch(e){toast(e.message,'err')}};
    if(id==='execution')$('cancel-quotes').onclick=async()=>{
      if(!selectedConfig()){toast('Choose one current bot before cancelling its quotes','err');return}
      const data=await jf('/api/v4/maker_fleet');const quotes=data.quotes.filter(q=>q.instance===S.inst&&['submitted','acked','partial'].includes(q.status));
      if(!confirm(`CANCEL OPEN QUOTES\nInstance: ${S.inst}\nRecorded open quotes: ${quotes.length}\nRecorded notional: ${money(quotes.reduce((a,q)=>a+q.resting_notional,0))}\nThe bot will request cancellation of all its open quotes. Fills may race cancellation. Continue?`))return;
      try{await jf('/api/v3/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({instance:S.inst,op:'cancel_open_orders'})});toast('Cancellation queued; inspect quote outcomes')}catch(e){toast(e.message,'err')}
    };
  }
  setInterval(()=>{const id=location.hash.slice(1);if(views[id]&&!['simulator','replay','configuration'].includes(id)&&!document.querySelector('#'+id+'-body input:focus'))load(id)},15_000);
  const oldBuild=buildCmds;
  buildCmds=function(){oldBuild();for(const[id,title]of Object.entries(views))CMDS.push({label:title,kbd:'view',fn:()=>location.hash='#'+id})};
  route();
})();
