const form = document.getElementById('repo-form');
const input = document.getElementById('repo-input');
const repoList = document.getElementById('repo-list');
const outputPanel = document.getElementById('output-panel');
const inputError = document.getElementById('input-error');
const tokenInput = document.getElementById('token-input');

const API = 'http://localhost:8080';

// { url: string, results: object[] }[]
let repos = [];
let activeIndex = -1;

function isValidGithubUrl(url) {
  return /^https:\/\/github\.com\/[^/]+\/[^/]+/.test(url.trim());
}

// ── Sidebar ───────────────────────────────────────────────────────────────────

function renderSidebar() {
  repoList.innerHTML = '';
  if (repos.length === 0) {
    repoList.innerHTML = '<li class="repo-empty">No repos yet</li>';
    return;
  }
  repos.forEach((repo, i) => {
    const li = document.createElement('li');
    li.className = 'repo-item' + (i === activeIndex ? ' active' : '');
    li.textContent = repo.url.replace('https://github.com/', '');
    li.title = repo.url;
    li.addEventListener('click', () => selectRepo(i));
    repoList.appendChild(li);
  });
}

function selectRepo(index) {
  activeIndex = index;
  renderSidebar();
  renderResults(repos[index].results);
}

// ── Output rendering ──────────────────────────────────────────────────────────

function severityLabel(s) {
  return s.charAt(0).toUpperCase() + s.slice(1);
}

function severityClass(s) {
  return 'sev-' + s;
}

function renderResults(results) {
  outputPanel.innerHTML = '';
  if (!results || results.length === 0) {
    outputPanel.innerHTML = '<span class="output-placeholder">Waiting for results...</span>';
    return;
  }
  results.forEach(r => outputPanel.appendChild(buildFileCard(r)));
}

function buildFileCard(r) {
  const card = document.createElement('div');
  card.className = 'file-card';

  const title = document.createElement('div');
  title.className = 'file-title';
  title.textContent = r.filePath ?? 'Unknown file';
  card.appendChild(title);

  if (r.status === 'error') {
    const err = document.createElement('p');
    err.className = 'result-error';
    err.textContent = 'Analysis failed for this file.';
    card.appendChild(err);
    return card;
  }

  if (!r.issues || r.issues.length === 0) {
    const none = document.createElement('p');
    none.className = 'result-desc';
    none.textContent = 'No issues found.';
    card.appendChild(none);
    return card;
  }

  // Group issues by category
  const groups = {};
  r.issues.forEach(issue => {
    const cat = issue.category ?? 'other';
    if (!groups[cat]) groups[cat] = [];
    groups[cat].push(issue);
  });

  Object.entries(groups).forEach(([cat, issues]) => {
    const section = document.createElement('div');
    section.className = 'result-section';

    const h = document.createElement('div');
    h.className = 'result-section-title';
    h.textContent = cat.charAt(0).toUpperCase() + cat.slice(1);
    section.appendChild(h);

    issues.forEach(issue => {
      const row = document.createElement('div');
      row.className = 'result-row';

      const badge = document.createElement('span');
      badge.className = 'sev-badge ' + severityClass(issue.severity);
      badge.textContent = severityLabel(issue.severity);
      row.appendChild(badge);

      const desc = document.createElement('span');
      desc.className = 'result-desc';
      desc.textContent = (issue.line ? `L${issue.line}: ` : '') + issue.description;
      row.appendChild(desc);

      section.appendChild(row);
    });

    card.appendChild(section);
  });

  return card;
}

// ── Backend calls ─────────────────────────────────────────────────────────────

async function debugRepo(url, sessionId, repoEntry) {
  // 1. Open SSE stream first
  const es = new EventSource(`${API}/stream?session_id=${sessionId}`);
  es.onmessage = (e) => {
    try {
      const result = JSON.parse(e.data);
      if (result.event === 'done') { es.close(); return; }
      repoEntry.results.push(result);
      if (repos[activeIndex] === repoEntry) renderResults(repoEntry.results);
    } catch (_) { /* ignore malformed */ }
  };

  // 2. Kick off clone + analysis
  const res = await fetch(`${API}/debug`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ url, session_id: sessionId, token: tokenInput.value.trim() }),
  });

  if (!res.ok) {
    es.close();
    return `Error: ${await res.text()}`;
  }

  await res.json();
}

// ── Form submit ───────────────────────────────────────────────────────────────

form.addEventListener('submit', async (e) => {
  e.preventDefault();
  const url = input.value.trim();

  if (!isValidGithubUrl(url)) {
    inputError.classList.remove('hidden');
    return;
  }
  inputError.classList.add('hidden');

  const existing = repos.findIndex(r => r.url === url);
  if (existing !== -1) { selectRepo(existing); input.value = ''; return; }

  const sessionId = crypto.randomUUID();
  const entry = { url, results: [] };
  repos.unshift(entry);
  activeIndex = 0;
  renderSidebar();
  renderResults([]);
  input.value = '';

  await debugRepo(url, sessionId, entry);
});

// Initial render
renderSidebar();
