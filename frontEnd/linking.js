const form = document.getElementById('repo-form');
const input = document.getElementById('repo-input');
const repoList = document.getElementById('repo-list');
const outputPanel = document.getElementById('output-panel');
const inputError = document.getElementById('input-error');

// { url: string, output: string }[]
let repos = [];
let activeIndex = -1;

function isValidGithubUrl(url) {
  return /^https:\/\/github\.com\/[^/]+\/[^/]+/.test(url.trim());
}

function renderSidebar() {
  repoList.innerHTML = '';

  if (repos.length === 0) {
    repoList.innerHTML = '<li class="repo-empty">No repos yet</li>';
    return;
  }

  repos.forEach((repo, i) => {
    const li = document.createElement('li');
    li.className = 'repo-item' + (i === activeIndex ? ' active' : '');
    // Show just "user/repo" for brevity
    li.textContent = repo.url.replace('https://github.com/', '');
    li.title = repo.url;
    li.addEventListener('click', () => selectRepo(i));
    repoList.appendChild(li);
  });
}

function selectRepo(index) {
  activeIndex = index;
  renderSidebar();
  outputPanel.textContent = repos[index].output;
}

function setOutput(text, isLoading = false) {
  outputPanel.innerHTML = '';
  const span = document.createElement('span');
  span.className = isLoading ? 'output-loading' : '';
  span.textContent = text;
  outputPanel.appendChild(span);
}

async function debugRepo(url) {
  setOutput(`Analyzing ${url} ...`, true);

  // TODO: replace with real backend call
  // const res = await fetch('/debug', { method: 'POST', body: JSON.stringify({ url }), headers: { 'Content-Type': 'application/json' } });
  // const data = await res.json();
  // return data.output;

  // Placeholder response until backend is wired up
  await new Promise(r => setTimeout(r, 800));
  return `Repository: ${url}\n\nDebug output will appear here once the backend is connected.`;
}

form.addEventListener('submit', async (e) => {
  e.preventDefault();
  const url = input.value.trim();

  if (!isValidGithubUrl(url)) {
    inputError.classList.remove('hidden');
    return;
  }
  inputError.classList.add('hidden');

  // Add to list (avoid exact duplicates)
  const existing = repos.findIndex(r => r.url === url);
  if (existing !== -1) {
    selectRepo(existing);
    input.value = '';
    return;
  }

  const entry = { url, output: '' };
  repos.unshift(entry);
  activeIndex = 0;
  renderSidebar();
  input.value = '';

  const output = await debugRepo(url);
  repos[0].output = output;
  if (activeIndex === 0) setOutput(output);
  renderSidebar();
});

// Initial render
renderSidebar();
