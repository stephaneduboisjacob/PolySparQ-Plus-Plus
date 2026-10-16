/* Fleet control plane. Mutations use validated, audited server workflows. */
(() => {
  const money=v=>'$'+Number(v||0).toFixed(2);
  const fleetLink=document.querySelector('nav [data-v=fleet]');fleetLink.innerHTML='<span class="ic">⚙</span>Fleet · Manage Bots';
  document.querySelector('nav [data-v=positions]').innerHTML='<span class="ic">▤</span>Live Positions & History';
  const fleet=$('v-fleet');for(const el of [...fleet.children])if(!el.classList.contains('v4-hero'))el.hidden=true;
  fleet.insertAdjacentHTML('beforeend',`<div id="fleet-manager"><p class="sub">Start, stop, supervise and configure each bot. Process status and entry status are shown separately.</p>
    <div style="display:flex;gap:10px;margin:12px 0"><button id="fleet-create-open" class="primary">＋ Create bot</button><button id="fleet-refresh">Refresh fleet</button><span id="fleet-fresh" class="mut"></span></div>
    <form id="fleet-create" class="card" hidden><h2>Create a PAPER bot</h2><label>Unique bot ID <input name="instance" pattern="[A-Za-z0-9][A-Za-z0-9_-]{0,39}" required placeholder="btc-maker-2"></label> <label>Clone settings from <select name="template" id="fleet-template"></select></label> <label>Execution <select name="execution_mode"><option value="maker">MAKER · POST-ONLY</option></select></label> <button class="primary">Create stopped bot</button><p class="mut">Created stopped with entry automation disabled. Every order uses a post-only limit quote and can remain unfilled.</p></form>
    <div id="fleet-cards" class="grid g3"></div><div id="fleet-config" style="margin-top:18px"></div></div>`);
  let bots=[],busy=false,editGeneration=0;
  const post=body=>jf('/api/v4/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'fleet',...body})});
  const liveConfirm=(b,verb)=>b.configured_mode!=='live'||prompt(`${verb} LIVE bot ${b.instance}. This can place real-money orders. Type LIVE ${b.instance} to confirm.`)===`LIVE ${b.instance}`;
  async function refresh(){
    if(busy)return;busy=true;
    try{
      const data=await jf('/api/v4/fleet_management');bots=data.fleet;
      $('fleet-fresh').textContent='Updated '+new Date(data.server_ts).toLocaleTimeString()+' · statistics: last 24h';
      const template=$('fleet-template').value;
      $('fleet-template').innerHTML=bots.map(b=>`<option value="${esc(b.instance)}">${esc(b.instance)}</option>`).join('');
      if(bots.some(b=>b.instance===template))$('fleet-template').value=template;
      $('fleet-cards').innerHTML=bots.map(b=>{
        const p=b.performance,entry=b.daily_kill_fired?'LOSS LIMIT — ENTRIES BLOCKED':b.autopilot===true?'ENTRIES ON':b.autopilot===false?'ENTRIES PAUSED':b.pid?'ENTRY STATE UNVERIFIED':'STOPPED';
        return `<article class="card"><h2>${esc(b.instance)} <span class="tag">${esc(b.configured_mode.toUpperCase())}</span> <span class="tag">${esc(b.execution_mode.toUpperCase())}</span></h2><p><b>${esc(b.status.toUpperCase())}</b> · ${esc(entry)}</p>${b.recycler_mode&&b.recycler_mode!=='off'?`<p><b>${esc(b.recycler_mode.toUpperCase())} RECYCLER</b><br><small>Entries and exits use post-only quotes; unfilled exits remain open until cancelled or settlement.</small></p>`:''}<p>Supervisor: <b>${b.supervisor_running?'ON · may tune settings':'OFF'}</b></p><p><b>${b.entries_24h} entries</b> · <span class="pos">${p.wins||0} WIN</span> / <span class="neg">${(p.trades||0)-(p.wins||0)-(p.breakeven||0)} LOSS</span> · Net <b class="${cl(p.net)}">${fmt$(p.net||0)}</b></p>
        <div style="display:flex;flex-wrap:wrap;gap:6px"><button data-op="${b.pid?'stop':'start'}" data-bot="${esc(b.instance)}">${b.pid?'■ Stop':'▶ Start'}</button><button data-op="restart" data-bot="${esc(b.instance)}">Restart</button><button data-op="entries" data-bot="${esc(b.instance)}" ${!b.pid?'disabled':''}>${b.autopilot===true?'Pause entries':'Resume entries'}</button><button data-op="supervisor" data-bot="${esc(b.instance)}">${b.supervisor_running?'Stop':'Enable'} supervisor</button><button data-op="quotes" data-bot="${esc(b.instance)}" ${!b.pid?'disabled':''}>Cancel quotes</button><button class="primary" data-edit="${esc(b.instance)}">Configure ${esc(b.execution_mode.toUpperCase())}</button></div>
        <details style="margin-top:10px"><summary>Why no entry? Latest observed gates</summary><p class="mut">${esc(b.diagnostics.message||'No recent rejection captured.')}</p><pre style="white-space:pre-wrap;font-size:10px">${esc(b.diagnostics.heartbeat||'No recent heartbeat in log.')}</pre><small>${esc(b.diagnostics.source||'')}</small></details></article>`;
      }).join('');
      $('fleet-cards').querySelectorAll('[data-op]').forEach(button=>button.onclick=()=>operate(button));
      $('fleet-cards').querySelectorAll('[data-edit]').forEach(button=>button.onclick=()=>selectBot(button.dataset.edit));
      // Keep the single top-bar selector synchronized after provisioning.
      S.fleet=bots;S.instances=[...bots,...(S.instances||[]).filter(b=>b.status==='historical'&&!bots.some(x=>x.instance===b.instance))];
      $('selInstance').innerHTML='<option value="">ALL BOTS</option>'+S.instances.map(b=>`<option value="${esc(b.instance)}" ${b.instance===S.inst?'selected':''}>${esc(b.instance)}${b.status==='historical'?' · history':''}</option>`).join('');
      setMode(bots.find(b=>b.instance===S.inst));
    }catch(e){$('fleet-fresh').textContent='UNAVAILABLE: '+e.message;toast(e.message,'err')}finally{busy=false}
  }
  async function operate(button){
    const b=bots.find(x=>x.instance===button.dataset.bot),op=button.dataset.op;button.disabled=true;
    try{
      if(op==='quotes'){
        if(!confirm(`CANCEL all resting quotes for ${b.instance} (${b.configured_mode.toUpperCase()}). Fills may race cancellation. Continue?`))return;
        await jf('/api/v3/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({instance:b.instance,op:'cancel_open_orders'})});toast('Cancellation queued; inspect Maker Fleet outcomes.');
      }else if(op==='entries'){
        const enabled=b.autopilot!==true;
        if(enabled&&b.daily_kill_fired)throw new Error('Session loss limit reached. Review results and risk settings first. Restarting a flat PAPER bot begins a new recorded experiment.');
        if(enabled&&!liveConfirm(b,'Resume'))return;
        if(!confirm(`${enabled?'RESUME':'PAUSE'} entries for ${b.instance} (${b.configured_mode.toUpperCase()})? Existing positions remain managed.`))return;
        await jf('/api/v3/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({instance:b.instance,op:'autopilot',enabled,confirm_live:enabled&&b.configured_mode==='live'})});
        toast('Entry command queued; runtime status confirms the result.');
      }else{
        const enabling=op==='supervisor'&&!b.supervisor_running;
        const message=op==='supervisor'?`${enabling?'ENABLE':'STOP'} supervisor for ${b.instance}? ${enabling?'It can automatically change this bot’s strategy configuration.':'Bot trading continues with its current settings.'}`:
          `${op.toUpperCase()} ${b.instance} (${b.configured_mode.toUpperCase()})? ${op==='stop'?'Stops position management; this does not sell positions or guarantee venue quote cancellation.':op==='restart'?'PAPER starts a new accounting session; previous history remains. Resting orders and open positions require attention.':'Trading follows its configured autopilot-on-start setting.'}`;
        if(!confirm(message))return;
        if((['start','restart'].includes(op)||enabling)&&!liveConfirm(b,op))return;
        const result=await post({operation:op,instance:b.instance,enabled:enabling,confirmed:true,confirm_live:b.configured_mode==='live'});toast(result.message||'Service operation requested; check observed status');
      }
      await refresh();setTimeout(refresh,2500);
    }catch(e){toast(e.message,'err')}finally{button.disabled=false}
  }
  function selectBot(name){
    $('selInstance').value=name;$('selInstance').dispatchEvent(new Event('change'));if(S.inst===name)configure(name);
  }
  async function configure(name){
    const generation=++editGeneration;if(!name){$('fleet-config').innerHTML='<p class="warn-panel">Choose Configure on a bot to edit its properties.</p>';return}
    try{
      const k=await jf('/api/v3/knobs?instance='+encodeURIComponent(name));if(generation!==editGeneration||S.inst!==name)return;
      const b=bots.find(x=>x.instance===name);if(!b)return;
      const controls=k.groups.flatMap(g=>g.rows);
      function field(r){const attrs=`data-key="${esc(r.key)}" ${r.active?'':'disabled'}`;
        if(r.type==='bool')return `<input type="checkbox" ${attrs} ${r.value?'checked':''}>`;
        if(r.type==='enum')return `<select ${attrs}>${r.enum.map(v=>`<option ${r.value===v?'selected':''}>${esc(v)}</option>`).join('')}</select>`;
        return `<input ${attrs} value="${esc(r.type==='list'?JSON.stringify(r.value):r.value)}" style="width:210px" ${['int','float','num'].includes(r.type)?`type="number" step="${r.type==='int'?1:'any'}" ${r.lo!=null?'min="'+r.lo+'"':''} ${r.hi!=null?'max="'+r.hi+'"':''}`:'type="text"'}>`;
      }
      $('fleet-config').innerHTML=`<h2>Configure ${esc(name)}</h2><form id="fleet-properties" class="card"><h3>Runtime properties</h3><p class="mut">Stop the bot and supervisor, and reconcile open exposure before changing these. Saving does not start the bot.</p><label>Mode <select name="mode"><option value="paper" ${b.configured_mode==='paper'?'selected':''}>PAPER</option><option value="live" ${b.configured_mode==='live'?'selected':''} ${b.recycler_mode&&b.recycler_mode!=='off'?'disabled':''}>LIVE${b.recycler_mode&&b.recycler_mode!=='off'?' · unavailable for recycler':''}</option></select></label> <label>Paper starting balance $ <input name="paper_balance" type="number" min="10" max="1000000" value="${b.paper_balance}"></label> <button ${b.pid||b.supervisor_running?'disabled':''}>Save runtime properties</button></form>
        <form id="fleet-settings" novalidate class="card" style="margin-top:12px"><h3>Strategy, MAKER / TAKER execution and risk settings</h3><p class="mut">Changes are validated and saved to this bot only. MAKER stays passive; HYBRID explicitly permits crossing after its maker phase. Settings marked engine default can be saved into this profile.</p><button class="primary" style="position:sticky;top:0;z-index:1">Review and apply changes</button>
        ${k.groups.map(g=>`<details ${['Order Execution','Sizing & Exposure','Fees & Safety'].includes(g.group)?'open':''}><summary style="padding:12px;font-weight:bold">${esc(g.group)} · ${g.rows.length} settings</summary>${g.rows.map(r=>`<div style="display:flex;justify-content:space-between;gap:15px;align-items:center;padding:8px;border-bottom:1px solid var(--line)"><label>${esc(r.label)}<br><small class="mut">${esc(r.key)} · ${esc(r.source)}${r.restart_required?' · RESTART REQUIRED':''}</small><div class="mut">${esc(r.hint||'')}</div></label>${field(r)}</div>`).join('')}</details>`).join('')}</form>`;
      $('fleet-settings').onsubmit=async e=>{
        e.preventDefault();if(S.inst!==name)return;const changes=[];
        for(const input of e.target.querySelectorAll('[data-key]')){
          if(input.disabled)continue;const r=controls.find(r=>r.key===input.dataset.key);let value;
          try{value=r.type==='bool'?input.checked:r.type==='list'?JSON.parse(input.value):['int','float','num'].includes(r.type)?Number(input.value):input.value}catch{toast('Invalid array: '+r.key,'err');return}
          if(JSON.stringify(value)!==JSON.stringify(r.value))changes.push({key:r.key,value});
        }
        if(!changes.length){toast('No changes');return}
        if(!confirm(`APPLY ${changes.length} settings to ${name} (${b.configured_mode.toUpperCase()})\n`+changes.map(c=>`${c.key}: ${JSON.stringify(controls.find(r=>r.key===c.key).value)} → ${JSON.stringify(c.value)}`).join('\n')))return;
        const button=e.target.querySelector('button');button.disabled=true;
        try{await jf('/api/v3/config_write',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({instance:name,expected_hash:k.config_hash,changes,reason:'Fleet configuration editor'})});toast('Settings saved; hot controls reload automatically.');await refresh();configure(name)}catch(err){toast(err.message,'err')}finally{button.disabled=false}
      };
      $('fleet-properties').onsubmit=async e=>{
        e.preventDefault();const values=Object.fromEntries(new FormData(e.target)),changes={};
        if(values.mode!==b.configured_mode)changes.mode=values.mode;
        if(Number(values.paper_balance)!==b.paper_balance)changes.paper_balance=Number(values.paper_balance);
        if(!Object.keys(changes).length){toast('No changes');return}
        if(!confirm(`SAVE runtime properties for ${name}\n${JSON.stringify(changes,null,2)}\nNext start creates the selected execution session. Historical results are retained.`))return;
        if(changes.mode==='live'&&!liveConfirm({...b,configured_mode:'live'},'Configure'))return;
        try{const r=await post({operation:'properties',instance:name,changes,expected_hash:k.config_hash,confirmed:true,confirm_live:changes.mode==='live'});toast(r.message);await refresh();configure(name)}catch(err){toast(err.message,'err')}
      };
      $('fleet-config').scrollIntoView({behavior:'smooth',block:'start'});
    }catch(e){toast(e.message,'err')}
  }
  $('fleet-create-open').onclick=()=>{$('fleet-create').hidden=!$('fleet-create').hidden};
  $('fleet-create').onsubmit=async e=>{e.preventDefault();const button=e.target.querySelector('button');button.disabled=true;try{const values=Object.fromEntries(new FormData(e.target));const r=await post({operation:'create',...values});toast(r.message);await refresh();selectBot(values.instance);e.target.hidden=true}catch(err){toast(err.message,'err')}finally{button.disabled=false}};
  $('fleet-refresh').onclick=refresh;
  const previousFleet=loadFleet;loadFleet=function(full){return full?refresh():previousFleet(full)};
  addEventListener('botscopechange',()=>{if(location.hash==='#fleet')configure(S.inst)});
  addEventListener('hashchange',()=>{if(location.hash==='#fleet'){refresh();if(S.inst)configure(S.inst)}});
  if(location.hash==='#fleet'){refresh();}
})();
