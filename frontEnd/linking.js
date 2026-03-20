const form = document.getElementById('repo-form');
const input = document.getElementById('repo-input');
const repoList = document.getElementById('repo-list');
const outputPanel = document.getElementById('output-panel');
const inputError = document.getElementById('input-error');

const API = 'http://localhost:8080';

// Set your GitHub token here for private repos (leave empty for public)
const GITHUB_TOKEN = '';

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

function severityLabel(n) {
  const labels = ['', 'Low', 'Low-Med', 'Medium', 'High', 'Critical'];
  return labels[n] ?? n;
}

function severityClass(n) {
  if (n >= 5) return 'sev-critical';
  if (n >= 4) return 'sev-high';
  if (n >= 3) return 'sev-medium';
  return 'sev-low';
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
  title.textContent = r.file ?? 'Unknown file';
  card.appendChild(title);

  if (r.error) {
    const err = document.createElement('p');
    err.className = 'result-error';
    err.textContent = r.error;
    card.appendChild(err);
    return card;
  }

  const sections = [
    { key: 'bugs',        label: 'Bugs',               hasSeverity: true },
    { key: 'security',    label: 'Security Issues',     hasSeverity: true },
    { key: 'suggestions', label: 'Suggestions',         hasSeverity: false },
  ];

  sections.forEach(({ key, label, hasSeverity }) => {
    const items = r[key];
    if (!items || items.length === 0) return;

    const section = document.createElement('div');
    section.className = 'result-section';

    const h = document.createElement('div');
    h.className = 'result-section-title';
    h.textContent = label;
    section.appendChild(h);

    items.forEach(item => {
      const row = document.createElement('div');
      row.className = 'result-row';

      if (hasSeverity) {
        const badge = document.createElement('span');
        badge.className = 'sev-badge ' + severityClass(item.severity);
        badge.textContent = severityLabel(item.severity);
        row.appendChild(badge);
      }

      const desc = document.createElement('span');
      desc.className = 'result-desc';
      desc.textContent = (item.line ? `L${item.line}: ` : '') + item.description;
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
      repoEntry.results.push(result);
      if (repos[activeIndex] === repoEntry) renderResults(repoEntry.results);
    } catch (_) { /* ignore malformed */ }
  };

  // 2. Kick off clone + analysis
  const res = await fetch(`${API}/debug`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ url, session_id: sessionId, token: GITHUB_TOKEN }),
  });

  if (!res.ok) {
    es.close();
    return `Error: ${await res.text()}`;
  }

  const { queued } = await res.json();

  // Close SSE once all results are in
  const waitForAll = () => {
    if (repoEntry.results.length >= queued) { es.close(); return; }
    setTimeout(waitForAll, 200);
  };
  waitForAll();
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
