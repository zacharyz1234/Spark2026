const form               = document.getElementById('repo-form');
const input              = document.getElementById('repo-input');
const repoList           = document.getElementById('repo-list');
const outputPanel        = document.getElementById('output-panel');
const inputError         = document.getElementById('input-error');
const tokenInput         = document.getElementById('token-input');
const fileSelectorSection = document.getElementById('file-selector-section');
const fileListEl         = document.getElementById('file-list');
const selectAllBtn       = document.getElementById('select-all-btn');
const deselectAllBtn     = document.getElementById('deselect-all-btn');
const scanBtn            = document.getElementById('scan-btn');
const selectedCount      = document.getElementById('selected-count');
const downloadBtn        = document.getElementById('download-btn');

const API = 'http://localhost:8080';

// { url: string, files: string[], results: object[] }[]
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
  renderFileSelector(repos[index].files ?? []);
  renderResults(repos[index].results);
}

// ── File Selector ─────────────────────────────────────────────────────────────

function getExt(path) {
  const parts = path.split('.');
  return parts.length > 1 ? parts.pop() : '';
}

function renderFileSelector(files) {
  fileListEl.innerHTML = '';

  if (!files || files.length === 0) {
    fileSelectorSection.classList.add('hidden');
    return;
  }

  fileSelectorSection.classList.remove('hidden');

  files.forEach(filePath => {
    const label = document.createElement('label');
    label.className = 'file-checkbox-item';

    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.value = filePath;
    cb.checked = true;
    cb.addEventListener('change', updateSelectionState);

    const name = document.createElement('span');
    name.className = 'file-checkbox-label';
    name.textContent = filePath;
    name.title = filePath;

    const ext = getExt(filePath);
    if (ext) {
      const badge = document.createElement('span');
      badge.className = 'file-ext';
      badge.textContent = ext;
      label.appendChild(cb);
      label.appendChild(name);
      label.appendChild(badge);
    } else {
      label.appendChild(cb);
      label.appendChild(name);
    }

    fileListEl.appendChild(label);
  });

  updateSelectionState();
}

function getCheckboxes() {
  return Array.from(fileListEl.querySelectorAll('input[type="checkbox"]'));
}

function updateSelectionState() {
  const boxes = getCheckboxes();
  const checked = boxes.filter(b => b.checked);
  selectedCount.textContent = `${checked.length} of ${boxes.length} file${boxes.length !== 1 ? 's' : ''} selected`;
  scanBtn.disabled = checked.length === 0;
}

selectAllBtn.addEventListener('click', () => {
  getCheckboxes().forEach(b => (b.checked = true));
  updateSelectionState();
});

deselectAllBtn.addEventListener('click', () => {
  getCheckboxes().forEach(b => (b.checked = false));
  updateSelectionState();
});

// ── Output rendering ──────────────────────────────────────────────────────────

function severityClass(s) { return 'sev-' + s; }
function severityLabel(s) { return s.charAt(0).toUpperCase() + s.slice(1); }

function renderResults(results) {
  outputPanel.innerHTML = '';
  if (!results || results.length === 0) {
    outputPanel.innerHTML = '<span class="output-placeholder">Debug output will appear here...</span>';
    downloadBtn.classList.add('hidden');
    return;
  }
  results.forEach(r => outputPanel.appendChild(buildFileCard(r)));
  downloadBtn.classList.remove('hidden');
}

function buildFileCard(r) {
  const card = document.createElement('div');
  card.className = 'file-card';

  const title = document.createElement('div');
  title.className = 'file-title';
  title.textContent = r.filePath ?? 'Unknown file';
  card.appendChild(title);

  if (r.fileSummary) {
    const summary = document.createElement('p');
    summary.className = 'file-summary';
    summary.textContent = r.fileSummary;
    card.appendChild(summary);
  }

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

async function fetchRepoFiles(url) {
  const res = await fetch(`${API}/files`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ url, token: tokenInput.value.trim() }),
  });
  if (!res.ok) return null;
  const data = await res.json();
  return data.files ?? [];
}

async function debugRepo(url, sessionId, selectedFiles, repoEntry) {
  const es = new EventSource(`${API}/stream?session_id=${sessionId}`);
  es.onmessage = (e) => {
    try {
      const result = JSON.parse(e.data);
      if (result.event === 'done') { es.close(); return; }
      repoEntry.results.push(result);
      if (repos[activeIndex] === repoEntry) renderResults(repoEntry.results);
    } catch (_) { /* ignore malformed */ }
  };

  const res = await fetch(`${API}/debug`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      url,
      session_id: sessionId,
      token: tokenInput.value.trim(),
      files: selectedFiles,
    }),
  });

  if (!res.ok) {
    es.close();
    outputPanel.innerHTML = `<span class="result-error">Error: ${await res.text()}</span>`;
  }
}

// ── Download ──────────────────────────────────────────────────────────────────

downloadBtn.addEventListener('click', () => {
  if (activeIndex === -1) return;
  const entry = repos[activeIndex];
  const repoSlug = entry.url.replace('https://github.com/', '').replace(/\//g, '_');

  // Build a plain-text report
  const lines = [`GitInspect Report — ${entry.url}`, `Generated: ${new Date().toISOString()}`, ''];

  entry.results.forEach(r => {
    lines.push(`FILE: ${r.filePath ?? 'Unknown'}`);
    if (r.fileSummary) lines.push(`Summary: ${r.fileSummary}`);
    if (r.status === 'error') {
      lines.push('  [Analysis failed]');
    } else if (!r.issues || r.issues.length === 0) {
      lines.push('  No issues found.');
    } else {
      r.issues.forEach(issue => {
        const loc = issue.line ? ` L${issue.line}` : '';
        lines.push(`  [${(issue.severity ?? 'info').toUpperCase()}]${loc} (${issue.category ?? 'other'}) ${issue.description}`);
      });
    }
    lines.push('');
  });

  const blob = new Blob([lines.join('\n')], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `gitinspect_${repoSlug}_${Date.now()}.txt`;
  a.click();
  URL.revokeObjectURL(url);
});

// ── Scan button ───────────────────────────────────────────────────────────────

scanBtn.addEventListener('click', async () => {
  if (activeIndex === -1) return;
  const entry = repos[activeIndex];
  const selectedFiles = getCheckboxes().filter(b => b.checked).map(b => b.value);
  if (selectedFiles.length === 0) return;

  entry.results = [];
  renderResults([]);
  outputPanel.innerHTML = '<span class="output-loading">Scanning selected files...</span>';

  const sessionId = crypto.randomUUID();
  await debugRepo(entry.url, sessionId, selectedFiles, entry);
});

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

  const entry = { url, files: [], results: [] };
  repos.unshift(entry);
  activeIndex = 0;
  renderSidebar();
  renderResults([]);
  fileSelectorSection.classList.add('hidden');
  input.value = '';

  outputPanel.innerHTML = '<span class="output-loading">Fetching file list...</span>';

  const files = await fetchRepoFiles(url);
  if (files === null) {
    outputPanel.innerHTML = '<span class="result-error">Failed to fetch repo files.</span>';
    return;
  }

  entry.files = files;
  renderFileSelector(files);
  outputPanel.innerHTML = '<span class="output-placeholder">Select files above and click "Scan Selected".</span>';
});

// Initial render
renderSidebar();
