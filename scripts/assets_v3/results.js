(() => {
  const classicV4=document.documentElement.dataset.cockpitVersion==='v4-classic';
  TITLES.results='Wins / Losses';
  const nav=document.createElement('a');nav.href='#results';nav.dataset.v='results';nav.innerHTML='<span class="ic">◉</span>Wins / Losses';
  document.querySelector('nav [data-v=positions]').after(nav);
  const section=document.createElement('section');section.id='v-results';section.className='view';
  const hero=classicV4
    ? '<div class="card"><small style="color:var(--orange);letter-spacing:.12em">PERFORMANCE · REALIZED OUTCOMES</small><h1 style="margin:8px 0">Wins / Losses</h1><p class="sub">Matched exits, fees, and loss review for the selected bot.</p></div>'
    : '<div class="v4-hero"><img class="v4-hero-art" src="/assets_v4/loss-review-art.webp?v=5.3" alt="Trade outcome review artwork"><div class="v4-hero-copy"><div class="v4-kicker">Performance · realized outcomes</div><div class="v4-title">Wins / Losses</div><div class="v4-desc">Matched exits, fees, and loss review across bots and sessions.</div></div><img class="v4-hero-full-logo" src="/assets_v4/polysparq_logo_full_with_name.webp?v=5.3" alt="PolySparQ++"></div>';
  section.innerHTML=hero+'<p class="sub">PAPER results are simulated; modes are identified on every exit.</p><label>Period <select id="results-days"><option value="0.0104166667">Last 15 minutes</option><option value="0.0208333333">Last 30 minutes</option><option value="0.0416666667">Last 1 hour</option><option value="0.125">Last 3 hours</option><option value="0.25">Last 6 hours</option><option value="0.5" selected>Last 12 hours</option><option value="1">Last 24 hours</option><option value="7">Last 7 days</option><option value="30">Last 30 days</option><option value="0">All history</option></select></label> <label style="margin-left:12px">Mode <select id="results-mode"><option value="">All modes</option><option value="paper">PAPER</option><option value="live">LIVE</option></select></label> <button id="results-refresh">Refresh</button><p id="results-fresh" class="mut"></p><div id="results-cards" class="grid g4" style="margin:15px 0"></div><div id="results-rows" class="card" style="overflow:auto"></div>';
  section.insertAdjacentHTML('beforeend',`<div class="card" style="margin-top:16px;overflow:auto">
    <h2>Loss review</h2><p class="mut">Complete recorded market-side exposures, including partial exits and fees. One bot in one market and session counts once. PAPER outcomes use simulated fills.</p>
    <label style="margin-left:12px">Compare entries around UTC time <input id="review-cutoff" type="datetime-local" aria-label="UTC entry cohort cutoff"></label>
    <button id="review-refresh">Analyze</button><p id="review-fresh" class="mut"></p>
    <div id="review-summary"></div><div id="review-bots"></div><div id="review-price"></div><div id="review-execution"></div><div id="review-calibration"></div><div id="review-clusters"></div><div id="review-cohorts"></div>
    <p class="mut">A small sample cannot establish an edge. Bots in the same market share an outcome. Before/after is descriptive; config versions are not verified per trade.</p>
  </div>`);
  document.querySelector('main').append(section);
  const positionHeader=document.createElement('div');positionHeader.id='position-live-status';positionHeader.className='warn-panel';positionHeader.textContent='Open positions update every 5 seconds. PAPER positions are simulated. For completed outcomes, open Wins / Losses.';
  $('tPos2').closest('.card').prepend(positionHeader);
  const originalPositions=renderPositions;
  renderPositions=function(positions){
    originalPositions(positions);
    $('position-live-status').textContent=(S.inst||'ALL BOTS')+' · '+positions.length+' open positions · Updated '+new Date().toLocaleTimeString()+'. PAPER positions are simulated. Refreshes every 5 seconds.';
    for(const id of ['tPos','tPos2']){
      const table=$(id);
      if(!table.querySelector('[data-manage-header]'))table.querySelector('thead tr').insertAdjacentHTML('beforeend','<th data-manage-header>Manage</th>');
      if(!positions.length){table.querySelector('tbody td').colSpan=8;continue}
      [...table.querySelectorAll('tbody tr')].forEach((row,i)=>{
        const p=positions[i],cell=row.insertCell(),button=document.createElement('button');button.textContent=p.awaiting_settlement?'Awaiting settlement':'Close position';button.disabled=!!p.awaiting_settlement;cell.append(button);
        button.onclick=async()=>{
          if(!confirm(`CLOSE ${p.mode||'UNKNOWN'} POSITION\nBot: ${p.instance}\nMarket: ${p.asset} ${p.hz} ${p.side}\nCost basis: $${Number(p.size).toFixed(2)}\nRequests an immediate marketable exit at available liquidity. Fill price can differ from the displayed mark. Continue?`))return;
          button.disabled=true;
          try{await jf('/api/force_sell',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({instance:p.instance,cid:p.cid,side:p.side})});toast('Exit queued; inspect subsequent fills.')}catch(e){toast(e.message,'err')}finally{button.disabled=false}
        };
      });
    }
  };
  function money(v){return v===null||v===undefined?'—':fmt$(v)}
  function reviewTable(headers,rows){return '<div style="overflow:auto"><table><thead><tr>'+headers.map(h=>'<th>'+esc(h)+'</th>').join('')+'</tr></thead><tbody>'+rows.join('')+'</tbody></table></div>'}
  function reviewRow(cells){return '<tr>'+cells.map(c=>'<td>'+esc(c)+'</td>').join('')+'</tr>'}
  function renderReview(r){
    const s=r.summary;
    $('review-fresh').textContent=`${r.scope} · ${r.mode_scope.toUpperCase()} · ${s.samples} completed exposures across ${s.unique_markets} markets · Updated ${new Date(r.server_ts).toLocaleTimeString()}`;
    $('review-summary').innerHTML=`<p><b>${esc(s.wins)} wins / ${esc(s.losses)} losses · Net ${esc(money(s.net))}</b> · Fees ${esc(money(s.fees))} · Average entry cost ${s.cost_per_share===null?'—':esc(s.cost_per_share.toFixed(3))}/share · Net proceeds ${s.net_proceeds_per_share===null?'—':esc(s.net_proceeds_per_share.toFixed(3))}/share. ${s.limited_sample?'LIMITED SAMPLE: fewer than 30 distinct markets.':''}</p>`;
    $('review-bots').innerHTML='<h3>By bot</h3>'+reviewTable(['Bot','Mode','Exposures','W / L','Net','ROI','Avg win','Avg loss','Fees'],r.instances.map(x=>reviewRow([x.instance,x.mode,x.samples,`${x.wins} / ${x.losses}`,money(x.net),x.roi_pct===null?'—':x.roi_pct.toFixed(1)+'%',money(x.average_win),money(x.average_loss),money(x.fees)])));
    $('review-price').innerHTML='<h3>Entry price bands</h3>'+reviewTable(['Bot','Mode','Paid per share','Exposures','W / L','Net'],r.entry_prices.map(x=>reviewRow([x.instance,x.mode,x.price_band,x.samples,`${x.wins} / ${x.losses}`,money(x.net)])));
    $('review-execution').innerHTML='<h3>Observed maker / taker entries</h3><p class="mut">Filled exposures only. Different signals and maker fill selection prevent a causal comparison.</p>'+reviewTable(['Mode','Entry execution','Exposures','Distinct markets','W / L','Net','Fees'],r.entry_execution.map(x=>reviewRow([x.mode,x.entry_execution,x.samples,x.unique_markets,`${x.wins} / ${x.losses}`,money(x.net),money(x.fees)])));
    const calibration=r.model_calibration;
    const pct=v=>v===null?'—':(100*v).toFixed(1)+'%';
    $('review-calibration').innerHTML='<h3>Predictor calibration · fully settled</h3><p class="mut">Stored predicted probability for the chosen side compared with the actual 0/1 result. Fewer than 30 distinct markets is a limited sample. Vote unanimity is not a 100% probability.</p>'+reviewTable(['Bot','Asset','Horizon','Markets','Mean predicted','Actual wins','Brier'],calibration.groups.map(x=>reviewRow([x.instance,x.asset,`${Number(x.horizon_seconds||0)/60}m`,x.distinct_markets,pct(x.mean_predicted),pct(x.realized_win_rate),x.brier_score===null?'—':x.brier_score.toFixed(3)])));
    $('review-clusters').innerHTML='<h3>Losses shared across bots</h3>'+(r.loss_clusters.length?reviewTable(['Market','Side','Bots','Exposures','Net'],r.loss_clusters.map(x=>reviewRow([`${x.asset||'?'} ${Number(x.horizon_seconds||0)/60}m`,x.side,x.instances.join(', '),x.samples,money(x.net)]))):'<p class="mut">No shared losing markets in this selection.</p>');
    const c=r.cohorts;
    $('review-cohorts').innerHTML=c?'<h3>Entries before / after UTC cutoff</h3>'+reviewTable(['Entry cohort','Exposures','W / L','Net','Distinct markets'],[['Before',c.before],['After',c.after],['Spanning cutoff',c.straddling]].map(([n,x])=>reviewRow([n,x.samples,`${x.wins} / ${x.losses}`,money(x.net),x.unique_markets]))):'';
  }
  let generation=0;
  async function load(){const seq=++generation,scope=S.inst,days=$('results-days').value,mode=$('results-mode').value;
    const periods={'0.0104166667':'15 minutes','0.0208333333':'30 minutes','0.0416666667':'1 hour','0.125':'3 hours','0.25':'6 hours','0.5':'12 hours','1':'24 hours','7':'7 days','30':'30 days','0':'All history'};
    try{const a=await jf(scopeUrl('/api/v4/analytics?days='+days+'&mode='+encodeURIComponent(mode)));if(seq!==generation||scope!==S.inst)return;
      const s=a.summary;$('results-cards').innerHTML=[['WIN',s.wins,'pos'],['LOSS',s.losses,'neg'],['BREAK EVEN',s.breakeven,''],['NET P&L',fmt$(s.net),cl(s.net)]].map(([label,value,cls])=>`<div class="card kpi"><small>${label}</small><div class="v ${cls}">${esc(value)}</div></div>`).join('');
      $('results-fresh').textContent=(scope||'ALL BOTS')+' · '+periods[days]+' · '+(mode?mode.toUpperCase():'ALL MODES')+' · '+s.trades+' matched exits · fees '+fmt$(s.fees)+' · '+a.excluded_unmatched_exits+' unmatched exits excluded · Updated '+new Date(a.server_ts).toLocaleTimeString();
      $('results-rows').innerHTML='<table><thead><tr><th>Time</th><th>Bot</th><th>Mode</th><th>Market</th><th>Side</th><th>Result</th><th>Net P&L</th><th>Exit reason</th></tr></thead><tbody>'+a.exits.slice(0,100).map(e=>`<tr><td>${esc(new Date(e.ts).toLocaleString())}</td><td>${esc(e.instance)}</td><td>${esc(e.mode.toUpperCase())}</td><td>${esc(e.asset)} ${Number(e.horizon_seconds||0)/60}m</td><td>${esc(e.side_label)}</td><td class="${cl(e.realized_pnl)}"><b>${e.realized_pnl>0?'WIN':e.realized_pnl<0?'LOSS':'BREAK EVEN'}</b></td><td class="${cl(e.realized_pnl)}">${fmt$(e.realized_pnl)}</td><td>${esc(e.source)}</td></tr>`).join('')+'</tbody></table><p class="mut">Showing the latest 100 matched exits. Change the period to review another time range.</p>';
      let url='/api/v4/loss_review?days='+days+'&mode='+encodeURIComponent(mode);
      if($('review-cutoff').value){const stamp=Date.parse($('review-cutoff').value+'Z');if(!Number.isFinite(stamp))throw Error('Invalid UTC cutoff');url+='&cohort_since_ms='+stamp}
      const review=await jf(scopeUrl(url));if(seq!==generation||scope!==S.inst)return;renderReview(review);
    }catch(e){$('review-fresh').textContent='UNAVAILABLE: '+e.message;$('results-fresh').textContent='Results request: '+e.message}
  }
  $('results-days').onchange=load;$('results-mode').onchange=load;$('results-refresh').onclick=load;$('review-refresh').onclick=load;
  addEventListener('hashchange',()=>{if(location.hash==='#results')load()});
  addEventListener('botscopechange',()=>{if(location.hash==='#results')load()});
  setInterval(()=>{if(location.hash==='#results')load()},10000);
  if(location.hash==='#results'){route();load()}
})();
