/* Durable history is independent of the bot's current paper-accounting session. */
(() => {
  const section=document.createElement('div');section.className='card';section.style.margin='16px 0';section.id='position-history';
  section.innerHTML=`<h2 class="sec">Position and transaction history</h2>
    <form id="history-filters" style="display:flex;gap:10px;flex-wrap:wrap;align-items:center">
      <label>View <select id="history-kind"><option value="transactions">All transactions</option><option value="closed">Closed exits / P&amp;L</option></select></label>
      <label>Period <select id="history-days"><option value="7">Last 7 days</option><option value="30">Last 30 days</option><option value="90">Last 90 days</option><option value="0" selected>All recorded history</option></select></label>
      <label>Mode <select id="history-mode"><option value="">All recorded modes</option><option value="paper">PAPER</option><option value="live">LIVE</option><option value="shadow">SHADOW</option><option value="replay">REPLAY</option></select></label>
      <label>Search <input id="history-search" placeholder="Asset, market, order, source or bot"></label><button>Search / refresh</button>
    </form><p id="history-fresh" class="sub" aria-live="polite"></p><div id="history-totals"></div>
    <p id="history-method" class="mut"></p><div style="overflow:auto"><table id="history-table"><thead></thead><tbody></tbody></table></div>
    <button id="history-more" hidden style="margin-top:12px">Load more</button>`;
  $('v-positions').insertBefore(section,$('tAnalytics').closest('.card'));
  let generation=0,cursor=null,rows=[],lastScope=null;
  const money=v=>'$'+Number(v||0).toFixed(2),num=v=>Number(v||0).toFixed(3);
  const columns=closed=>[['Time','ts',v=>esc(new Date(v).toLocaleString())],['Bot','instance'],['Mode','mode',v=>esc(String(v||'').toUpperCase())],['Asset','asset'],['Horizon','horizon_seconds',v=>v?esc(v/60+'m'):'—'],['Side','side_label'],
    ...(closed?[['Result','realized_pnl',v=>`<strong class="${cl(v)}">${v>0?'WIN':v<0?'LOSS':'BREAK EVEN'}</strong>`],['Shares','shares',num],['Entry cost','entry_cost',money],['Exit proceeds','gross_proceeds',money],['Entry + exit fees','fees',money],['Net P&L','realized_pnl',v=>`<b class="${cl(v)}">${fmt$(v)}</b>`]]:
      [['Action','direction'],['Price','price',num],['Shares','size_shares',num],['Fee','fee_usd',money],['Cash flow','cash_flow',money]]),
    ['Source / exit reason','source'],['Execution','execution_mode'],['Recorded identifiers','client_order_id',(v,r)=>`<details><summary>Order / session / market</summary><div>Order: ${esc(v)}<br>Session: ${esc(r.session_id||'not recorded')}<br>Market: ${esc(r.condition_id)}</div></details>`]];
  async function load(more=false){
    const gen=++generation,scope=S.inst,closed=$('history-kind').value==='closed';
    if(!more){cursor=null;rows=[];$('history-table').querySelector('tbody').innerHTML='';$('history-totals').textContent='';}
    const params=new URLSearchParams({instance:scope,days:$('history-days').value,mode:$('history-mode').value,search:$('history-search').value,limit:'100'});
    if(more&&cursor)params.set('cursor',cursor);
    $('history-more').disabled=true;$('history-fresh').textContent='Loading '+(scope||'ALL BOTS')+'…';
    try{
      const data=await jf('/api/v4/'+(closed?'closed_positions':'transactions')+'?'+params);
      if(gen!==generation||scope!==S.inst)return;
      rows.push(...(closed?data.exits:data.transactions));cursor=data.next_cursor;
      const cols=columns(closed);$('history-table').querySelector('thead').innerHTML='<tr>'+cols.map(c=>'<th>'+esc(c[0])+'</th>').join('')+'</tr>';
      $('history-table').querySelector('tbody').innerHTML=rows.map(r=>'<tr>'+cols.map(c=>'<td>'+(c[2]?c[2](r[c[1]],r):esc(r[c[1]]??'—'))+'</td>').join('')+'</tr>').join('')||`<tr><td colspan="${cols.length}" class="empty">No recorded history for these filters.</td></tr>`;
      const t=data.totals;$('history-totals').textContent=closed?`${t.trades} matched exits · net ${money(t.net)} · fees ${money(t.fees)} · win rate ${t.wr==null?'—':t.wr.toFixed(1)+'%'}`:`${t.fills.toLocaleString()} recorded fills · purchases ${money(t.purchased)} · proceeds ${money(t.proceeds)} · fees ${money(t.fees)}`;
      $('history-method').textContent=closed?data.method+` Unmatched exits excluded from P&L: ${data.excluded_unmatched_exits}. All fills remain in All transactions.`:'All recorded sessions, including previous bot runs. Cash flow is the signed transaction amount after fees; it is not profit. Settlement transactions retain their recorded source.';
      $('history-fresh').textContent=`${scope||'ALL BOTS'} · ${rows.length} rows shown · Last updated ${new Date(data.server_ts).toLocaleString()}`;
      $('history-more').hidden=!cursor;lastScope=scope;
    }catch(e){if(gen===generation){$('history-fresh').textContent='HISTORY UNAVAILABLE · '+e.message;$('history-more').hidden=true;}}
    finally{if(gen===generation)$('history-more').disabled=false;}
  }
  $('history-filters').onsubmit=e=>{e.preventDefault();load()};
  for(const id of ['history-kind','history-days','history-mode'])$(id).onchange=()=>load();
  $('history-more').onclick=()=>load(true);
  addEventListener('botscopechange',()=>{lastScope=null;if(location.hash==='#positions')load()});
  addEventListener('hashchange',()=>{if(location.hash==='#positions')load()});
  if(location.hash==='#positions')load();
})();
