/* Compact V4 workspace navigation. Legacy hashes remain valid deep links. */
(() => {
  const workspaces = [
    { id: 'command', title: 'Command Center', icon: '⌂', home: 'overview', pages: [['overview', 'Overview'], ['monitor', 'Live monitor']] },
    { id: 'markets', title: 'Markets', icon: '◈', home: 'markets', pages: [['markets', 'Markets'], ['opportunities', 'Opportunities'], ['board', 'Prediction board']] },
    { id: 'portfolio', title: 'Portfolio', icon: '▤', home: 'positions', pages: [['positions', 'Positions & activity']] },
    { id: 'fleet', title: 'Fleet', icon: '⌘', home: 'fleet', pages: [['fleet', 'Instances'], ['maker', 'Orders & quotes'], ['orders', 'Order history'], ['execution', 'Execution quality']] },
    { id: 'strategies', title: 'Strategies', icon: '⟁', home: 'editor', pages: [['editor', 'Controls'], ['lab', 'Strategy lab'], ['decisions', 'Decision traces'], ['configuration', 'Versions']] },
    { id: 'performance', title: 'Performance', icon: '↗', home: 'analytics', pages: [['analytics', 'Analytics'], ['results', 'Wins & losses']] },
    { id: 'research', title: 'Research', icon: '⌕', home: 'research', pages: [['research', 'Research lab'], ['simulator', 'Simulator'], ['replay', 'Replay']] },
    { id: 'operations', title: 'Operations', icon: '◉', home: 'risk', pages: [['risk', 'Risk'], ['feeds', 'Feeds & system'], ['incidents', 'Incidents'], ['alerts', 'Alerts'], ['system', 'Host health'], ['logs', 'Logs'], ['audit', 'Audit'], ['account', 'Wallet']] },
  ];
  const pageAliases = { timetravel: 'replay' };
  const workspaceForPage = new Map();
  for (const workspace of workspaces) {
    for (const [page] of workspace.pages) workspaceForPage.set(page, workspace);
  }

  const nav = document.querySelector('#shell > nav');
  const main = document.querySelector('#shell > main');
  if (!nav || !main) return;
  const cockpitVersion = document.documentElement.dataset.cockpitVersion || 'v4';

  const sideCard = nav.querySelector('#sideCard') || document.querySelector('#sideCard');
  nav.innerHTML = workspaces.map(workspace => `
    <a class="workspace-link" href="#${workspace.home}" data-v="${workspace.home}" data-workspace="${workspace.id}">
      <span class="ic" aria-hidden="true">${workspace.icon}</span><span>${workspace.title}</span>
      ${workspace.id === 'operations' ? '<span class="workspace-alert-count" id="alertN"></span>' : ''}
    </a>`).join('') +
    '<div class="spacer"></div>' +
    '<button type="button" class="workspace-assistant" id="workspaceAssistant"><span aria-hidden="true">✦</span> PolyTron assistant</button>';
  if (sideCard) nav.append(sideCard);

  // The per-bot matched-exit scorecard belongs with performance analytics,
  // alongside the fleet-wide attribution and wins/losses workspace.
  const analyticsTable = document.getElementById('tAnalytics');
  const performanceView = document.getElementById('v-analytics');
  const performanceHero = performanceView?.querySelector(':scope > .v4-hero');
  const analyticsSummary = analyticsTable?.closest('.card');
  if (performanceHero && analyticsSummary && analyticsSummary.parentElement?.id !== 'v-analytics') {
    performanceHero.after(analyticsSummary);
  }

  let toolbar = document.getElementById('workspaceNav');
  if (!toolbar) {
    toolbar = document.createElement('div');
    toolbar.id = 'workspaceNav';
    toolbar.className = 'workspace-nav';
    const alertBanner = main.querySelector('#alertBanner');
    if (alertBanner) alertBanner.after(toolbar);
    else main.prepend(toolbar);
  }
  toolbar.innerHTML = `
    <div class="workspace-nav__desktop">
      <div class="workspace-nav__identity"><span>WORKSPACE</span><strong id="workspaceName"></strong></div>
      <div class="workspace-nav__tabs" id="workspaceTabs" role="navigation" aria-label="Workspace sections"></div>
    </div>
    <div class="workspace-nav__mobile">
      <label><span>WORKSPACE</span><select id="workspacePicker" aria-label="Choose workspace"></select></label>
      <label><span>SECTION</span><select id="workspaceSectionPicker" aria-label="Choose section"></select></label>
    </div>`;

  const workspacePicker = document.getElementById('workspacePicker');
  const sectionPicker = document.getElementById('workspaceSectionPicker');
  workspacePicker.innerHTML = workspaces.map(item => `<option value="${item.id}">${item.title}</option>`).join('');
  workspacePicker.addEventListener('change', () => {
    const target = workspaces.find(item => item.id === workspacePicker.value);
    if (target) location.hash = `#${target.home}`;
  });
  sectionPicker.addEventListener('change', () => { location.hash = `#${sectionPicker.value}`; });
  document.getElementById('workspaceAssistant').addEventListener('click', () => {
    if (typeof window.ptToggle === 'function') window.ptToggle(true);
  });

  const priorRoute = window.route;
  if (typeof priorRoute === 'function') {
    removeEventListener('hashchange', priorRoute);
    window.route = function workspaceAwareRoute() {
      const requested = (location.hash || '#overview').slice(1);
      if (pageAliases[requested]) {
        location.hash = `#${pageAliases[requested]}`;
        return;
      }
      priorRoute();
      const pageId = requested;
      if (pageId === 'analytics' && typeof window.loadAnalytics === 'function') window.loadAnalytics();
      renderWorkspaceNavigation();
    };
    addEventListener('hashchange', window.route);
    if (pageAliases[(location.hash || '').slice(1)]) location.hash = `#${pageAliases[location.hash.slice(1)]}`;
  }

  function renderWorkspaceNavigation() {
    const pageId = (location.hash || '#overview').slice(1);
    const workspace = workspaceForPage.get(pageId) || workspaces[0];
    const page = workspace.pages.find(([id]) => id === pageId) || workspace.pages[0];

    document.querySelectorAll('.workspace-link').forEach(link => {
      const active = link.dataset.workspace === workspace.id;
      link.classList.toggle('on', active);
      if (active) link.setAttribute('aria-current', 'page');
      else link.removeAttribute('aria-current');
    });
    document.getElementById('workspaceName').textContent = workspace.title;
    document.getElementById('workspaceTabs').innerHTML = workspace.pages.map(([id, label]) =>
      `<a href="#${id}" class="workspace-tab${id === page[0] ? ' on' : ''}"${id === page[0] ? ' aria-current="page"' : ''}>${label}</a>`
    ).join('');
    workspacePicker.value = workspace.id;
    sectionPicker.innerHTML = workspace.pages.map(([id, label]) => `<option value="${id}">${label}</option>`).join('');
    sectionPicker.value = page[0];
    const title = document.getElementById('scrTitle');
    if (title) title.textContent = `Cockpit ${cockpitVersion} · ${workspace.title}`;
  }

  renderWorkspaceNavigation();
})();
