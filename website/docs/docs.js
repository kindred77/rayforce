document.addEventListener('DOMContentLoaded', () => {
  'use strict';

  /* ── Search index: lazy-loaded on first input focus ──────────────────── */
  let searchIndex = null;
  let searchIndexLoading = null;
  function loadSearchIndex() {
    if (searchIndex) return Promise.resolve(searchIndex);
    if (searchIndexLoading) return searchIndexLoading;
    searchIndexLoading = fetch('search-index.json')
      .then(r => r.ok ? r.json() : Promise.reject(new Error('search-index ' + r.status)))
      .then(data => { searchIndex = data; return data; })
      .catch(err => { searchIndexLoading = null; console.error('[search] failed:', err); return []; });
    return searchIndexLoading;
  }

  const sidebar = document.querySelector('.docs-sidebar');
  if (!sidebar) return;
  const sections = sidebar.querySelectorAll('.sidebar-section');

  /* ── Search box ─────────────────────────────────────────────────────── */
  const searchBox = document.createElement('div');
  searchBox.className = 'sidebar-search';
  const isMac = /Mac|iPad|iPhone/.test(navigator.platform);
  const kbdLabel = isMac ? '⌘K' : 'Ctrl K';
  searchBox.innerHTML =
    '<div class="search-input-wrap">' +
      '<svg class="search-icon" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="2">' +
        '<circle cx="8.5" cy="8.5" r="5.5"/><line x1="13" y1="13" x2="18" y2="18"/>' +
      '</svg>' +
      '<input type="text" class="search-input" placeholder="Search docs..." aria-label="Search documentation">' +
      '<kbd class="search-kbd">' + kbdLabel + '</kbd>' +
    '</div>' +
    '<div class="search-results" style="display:none"></div>';
  sidebar.insertBefore(searchBox, sidebar.firstChild);

  const input = searchBox.querySelector('.search-input');
  const resultsEl = searchBox.querySelector('.search-results');
  const kbdEl = searchBox.querySelector('.search-kbd');

  /* Keyboard: '/' or Cmd/Ctrl+K to focus search; Esc to clear */
  document.addEventListener('keydown', (e) => {
    const isCmdK = (e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k';
    const isSlash = e.key === '/' && !e.ctrlKey && !e.metaKey && !e.altKey;
    if (isCmdK || isSlash) {
      const tag = document.activeElement.tagName;
      if (!isCmdK && (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT')) return;
      e.preventDefault();
      input.focus();
      input.select();
    }
    if (e.key === 'Escape' && document.activeElement === input) {
      input.blur(); input.value = ''; showNav();
    }
  });

  input.addEventListener('focus', () => { kbdEl.style.display = 'none'; });
  input.addEventListener('blur', () => { if (!input.value) kbdEl.style.display = ''; });

  function showNav() { resultsEl.style.display = 'none'; sections.forEach(s => s.style.display = ''); }
  function hideNav() { sections.forEach(s => s.style.display = 'none'); resultsEl.style.display = ''; }

  async function search(query) {
    const idx = await loadSearchIndex();
    if (!query) { showNav(); return; }
    hideNav();
    const terms = query.toLowerCase().split(/\s+/).filter(Boolean);
    const hits = [];
    for (const page of idx) {
      for (const sec of page.sections) {
        const haystack = (page.title + ' ' + sec.title + ' ' + sec.text).toLowerCase();
        const score = terms.reduce((s, t) => s + (haystack.includes(t) ? 1 : 0), 0);
        if (score === terms.length) hits.push({page, sec, score});
      }
    }
    if (hits.length === 0) { resultsEl.innerHTML = '<div class="search-empty">No results</div>'; return; }
    const byPage = {};
    for (const h of hits) {
      const key = h.page.url;
      if (!byPage[key]) byPage[key] = [];
      if (byPage[key].length < 2) byPage[key].push(h);
    }
    let html = '';
    for (const key in byPage) {
      const group = byPage[key];
      const page = group[0].page;
      html += '<div class="search-page"><div class="search-page-title">' + esc(page.title) + '</div>';
      for (const h of group) {
        const url = h.sec.id ? (page.url + '#' + h.sec.id) : page.url;
        html += '<a href="' + url + '" class="search-hit">' +
          '<span class="search-hit-title">' + highlight(h.sec.title, terms) + '</span>' +
          '<span class="search-hit-text">' + highlight(h.sec.text, terms) + '</span></a>';
      }
      html += '</div>';
    }
    resultsEl.innerHTML = html;
  }

  function esc(s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }
  function highlight(s, terms) {
    let out = esc(s);
    for (const t of terms) {
      const re = new RegExp('(' + t.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ')', 'gi');
      out = out.replace(re, '<mark>$1</mark>');
    }
    return out;
  }

  let searchTimer = null;
  input.addEventListener('focus', loadSearchIndex);
  input.addEventListener('input', () => {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(() => search(input.value.trim()), 150);
  });

  resultsEl.addEventListener('click', (e) => {
    if (e.target.closest('.search-hit')) closeSidebar();
  });

  /* ── Foldable sidebar sections (persisted in localStorage) ───────────── */
  const FOLD_KEY = 'rayforce:docs:folded';
  const folded = new Set(JSON.parse(localStorage.getItem(FOLD_KEY) || '[]'));
  const currentPage = window.location.pathname.split('/').pop() || 'index.html';

  sections.forEach((sec) => {
    const label = sec.querySelector('.sidebar-section-label');
    if (!label) return;
    const sectionKey = label.textContent.trim();

    // Wrap links into a collapsible container
    const links = Array.from(sec.querySelectorAll('.sidebar-link'));
    const items = document.createElement('div');
    items.className = 'sidebar-section-items';
    links.forEach(l => items.appendChild(l));
    sec.appendChild(items);

    // Auto-expand if any link in this section is the current page
    const containsActive = links.some(l => l.getAttribute('href') === currentPage);
    const startOpen = containsActive || !folded.has(sectionKey);

    label.classList.add('sidebar-section-toggle');
    label.setAttribute('role', 'button');
    label.setAttribute('tabindex', '0');
    label.setAttribute('aria-expanded', String(startOpen));
    label.innerHTML = '<svg class="sidebar-chevron" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="2"><polyline points="4 6 8 10 12 6"/></svg>' +
      '<span>' + label.textContent + '</span>';
    if (!startOpen) sec.classList.add('collapsed');

    function toggle() {
      const isCollapsed = sec.classList.toggle('collapsed');
      label.setAttribute('aria-expanded', String(!isCollapsed));
      if (isCollapsed) folded.add(sectionKey); else folded.delete(sectionKey);
      localStorage.setItem(FOLD_KEY, JSON.stringify([...folded]));
    }
    label.addEventListener('click', toggle);
    label.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggle(); }
    });
  });

  /* ── Highlight active page ──────────────────────────────────────────── */
  document.querySelectorAll('.sidebar-link').forEach((link) => {
    const href = link.getAttribute('href');
    if (href === currentPage || (currentPage === '' && href === 'index.html')) {
      link.classList.add('active');
    }
  });

  /* ── Mobile sidebar drawer with backdrop ────────────────────────────── */
  const toggle = document.querySelector('.docs-sidebar-toggle');
  let backdrop = document.querySelector('.docs-sidebar-backdrop');
  if (!backdrop) {
    backdrop = document.createElement('div');
    backdrop.className = 'docs-sidebar-backdrop';
    backdrop.setAttribute('aria-hidden', 'true');
    document.body.appendChild(backdrop);
  }
  function openSidebar() {
    sidebar.classList.add('open');
    backdrop.classList.add('open');
    if (toggle) toggle.setAttribute('aria-expanded', 'true');
    document.body.style.overflow = 'hidden';
  }
  function closeSidebar() {
    sidebar.classList.remove('open');
    backdrop.classList.remove('open');
    if (toggle) toggle.setAttribute('aria-expanded', 'false');
    document.body.style.overflow = '';
  }
  if (toggle) {
    toggle.addEventListener('click', () => {
      sidebar.classList.contains('open') ? closeSidebar() : openSidebar();
    });
  }
  backdrop.addEventListener('click', closeSidebar);
  sidebar.querySelectorAll('.sidebar-link').forEach((link) => {
    link.addEventListener('click', () => { if (window.innerWidth <= 768) closeSidebar(); });
  });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && sidebar.classList.contains('open')) closeSidebar();
  });

  /* ── On-this-page TOC (right rail, scrollspy) ───────────────────────── */
  const content = document.querySelector('.docs-content');
  if (content) {
    const tocHeadings = content.querySelectorAll('h2[id], h3[id]');
    if (tocHeadings.length >= 2) {
      const aside = document.createElement('aside');
      aside.className = 'docs-toc';
      aside.setAttribute('aria-label', 'On this page');
      aside.innerHTML = '<div class="docs-toc-label">On this page</div><ul></ul>';
      const ul = aside.querySelector('ul');
      tocHeadings.forEach((h) => {
        const li = document.createElement('li');
        li.className = 'docs-toc-item docs-toc-' + h.tagName.toLowerCase();
        const a = document.createElement('a');
        a.href = '#' + h.id;
        a.textContent = h.textContent.replace(/¶|#/g, '').trim();
        li.appendChild(a);
        ul.appendChild(li);
      });
      content.parentElement.appendChild(aside);

      // Scrollspy: highlight whichever heading is most visible
      const tocLinks = new Map();
      ul.querySelectorAll('a').forEach(a => tocLinks.set(a.getAttribute('href').slice(1), a));
      const obs = new IntersectionObserver((entries) => {
        entries.forEach(entry => {
          const link = tocLinks.get(entry.target.id);
          if (!link) return;
          if (entry.isIntersecting) link.classList.add('active');
          else link.classList.remove('active');
        });
      }, { rootMargin: '-80px 0px -65% 0px', threshold: 0 });
      tocHeadings.forEach(h => obs.observe(h));
    }
  }

  /* ── Heading anchor links (hover #) ─────────────────────────────────── */
  if (content) {
    content.querySelectorAll('h2[id], h3[id], h4[id]').forEach((h) => {
      const a = document.createElement('a');
      a.className = 'heading-anchor';
      a.href = '#' + h.id;
      a.setAttribute('aria-label', 'Link to this section');
      a.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/></svg>';
      h.appendChild(a);
      a.addEventListener('click', (e) => {
        // Let the hash navigation happen, then copy URL to clipboard
        const url = location.origin + location.pathname + '#' + h.id;
        if (navigator.clipboard && navigator.clipboard.writeText) {
          navigator.clipboard.writeText(url).then(() => flashAnchor(a));
        }
      });
    });
  }
  function flashAnchor(a) {
    a.classList.add('copied');
    setTimeout(() => a.classList.remove('copied'), 1200);
  }

  /* ── Smooth scroll for in-page anchor links ─────────────────────────── */
  document.querySelectorAll('a[href^="#"]').forEach((anchor) => {
    anchor.addEventListener('click', (e) => {
      const href = anchor.getAttribute('href');
      if (href.length < 2) return;
      const target = document.querySelector(href);
      if (target) {
        e.preventDefault();
        target.scrollIntoView({ behavior: 'smooth', block: 'start' });
        history.replaceState(null, '', href);
      }
    });
  });

  /* ── Copy button on every <pre> ─────────────────────────────────────── */
  document.querySelectorAll('.docs-content pre').forEach((pre) => {
    const btn = document.createElement('button');
    btn.className = 'code-copy-btn';
    btn.type = 'button';
    btn.setAttribute('aria-label', 'Copy code');
    btn.innerHTML = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true"><rect x="5" y="5" width="9" height="9" rx="1.5"/><path d="M3 11V3a1 1 0 0 1 1-1h8"/></svg><span>Copy</span>';
    pre.appendChild(btn);
    btn.addEventListener('click', () => {
      const code = pre.querySelector('code') || pre;
      const text = code.innerText;
      if (!navigator.clipboard) return;
      navigator.clipboard.writeText(text).then(() => {
        btn.classList.add('copied');
        const span = btn.querySelector('span');
        const old = span.textContent;
        span.textContent = 'Copied';
        setTimeout(() => { btn.classList.remove('copied'); span.textContent = old; }, 1500);
      });
    });
  });

  /* ── Edit-on-GitHub + Last updated footer ───────────────────────────── */
  if (content) {
    const filename = (location.pathname.split('/').pop() || 'index.html');
    const githubUrl = 'https://github.com/RayforceDB/rayforce/blob/master/website/docs/' + filename;

    // Try to extract dateModified from JSON-LD; else null.
    let lastUpdated = null;
    document.querySelectorAll('script[type="application/ld+json"]').forEach((script) => {
      try {
        const data = JSON.parse(script.textContent);
        const graph = data['@graph'] || [data];
        for (const node of graph) if (node.dateModified) lastUpdated = node.dateModified;
      } catch (_) {}
    });

    const meta = document.createElement('div');
    meta.className = 'docs-page-meta';
    let html = '<a class="docs-edit-link" href="' + githubUrl + '" target="_blank" rel="noopener">' +
      '<svg viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0016 8c0-4.42-3.58-8-8-8z"/></svg>' +
      'Edit this page on GitHub</a>';
    if (lastUpdated) {
      const d = new Date(lastUpdated);
      if (!isNaN(d)) {
        const fmt = d.toLocaleDateString(undefined, { year: 'numeric', month: 'long', day: 'numeric' });
        html += '<span class="docs-updated">Last updated <time datetime="' + lastUpdated + '">' + fmt + '</time></span>';
      }
    }
    meta.innerHTML = html;

    const navFooter = content.querySelector('.docs-nav-footer');
    if (navFooter) navFooter.parentElement.insertBefore(meta, navFooter);
    else content.appendChild(meta);
  }
});
