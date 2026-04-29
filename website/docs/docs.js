document.addEventListener('DOMContentLoaded', () => {
  'use strict';
  /* ── Search index: lazy-loaded on first input focus ── */
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

  /* ── Search Logic ────────────────────────────── */
  const sidebar = document.querySelector('.docs-sidebar');
  if (!sidebar) return;

  const sections = sidebar.querySelectorAll('.sidebar-section');
  const searchBox = document.createElement('div');
  searchBox.className = 'sidebar-search';
  searchBox.innerHTML =
    '<div class="search-input-wrap">' +
      '<svg class="search-icon" viewBox="0 0 20 20" fill="none" stroke="currentColor" stroke-width="2">' +
        '<circle cx="8.5" cy="8.5" r="5.5"/><line x1="13" y1="13" x2="18" y2="18"/>' +
      '</svg>' +
      '<input type="text" class="search-input" placeholder="Search docs..." aria-label="Search documentation">' +
      '<kbd class="search-kbd">/</kbd>' +
    '</div>' +
    '<div class="search-results" style="display:none"></div>';
  sidebar.insertBefore(searchBox, sidebar.firstChild);

  const input = searchBox.querySelector('.search-input');
  const resultsEl = searchBox.querySelector('.search-results');
  const kbdEl = searchBox.querySelector('.search-kbd');

  /* Keyboard shortcut: / to focus search */
  document.addEventListener('keydown', (e) => {
    if (e.key === '/' && !e.ctrlKey && !e.metaKey && !e.altKey) {
      const tag = document.activeElement.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
      e.preventDefault();
      input.focus();
    }
    if (e.key === 'Escape' && document.activeElement === input) {
      input.blur();
      input.value = '';
      showNav();
    }
  });

  input.addEventListener('focus', () => { kbdEl.style.display = 'none'; });
  input.addEventListener('blur', () => { if (!input.value) kbdEl.style.display = ''; });

  function showNav() {
    resultsEl.style.display = 'none';
    sections.forEach(s => s.style.display = '');
  }

  function hideNav() {
    sections.forEach(s => s.style.display = 'none');
    resultsEl.style.display = '';
  }

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
        if (score === terms.length) {
          hits.push({page, sec, score});
        }
      }
    }

    if (hits.length === 0) {
      resultsEl.innerHTML = '<div class="search-empty">No results</div>';
      return;
    }

    /* Deduplicate: show at most 2 sections per page */
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
      html += '<div class="search-page">' +
        '<div class="search-page-title">' + esc(page.title) + '</div>';
      for (const h of group) {
        const url = h.sec.id ? (page.url + '#' + h.sec.id) : page.url;
        html += '<a href="' + url + '" class="search-hit">' +
          '<span class="search-hit-title">' + highlight(h.sec.title, terms) + '</span>' +
          '<span class="search-hit-text">' + highlight(h.sec.text, terms) + '</span>' +
          '</a>';
      }
      html += '</div>';
    }
    resultsEl.innerHTML = html;
  }

  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

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

  /* Close mobile sidebar when a search result is clicked */
  resultsEl.addEventListener('click', (e) => {
    if (e.target.closest('.search-hit')) {
      sidebar.classList.remove('open');
    }
  });

  /* ── Mobile sidebar toggle ───────────────────── */
  const toggle = document.querySelector('.docs-sidebar-toggle');
  if (toggle && sidebar) {
    toggle.addEventListener('click', () => {
      const isOpen = sidebar.classList.toggle('open');
      toggle.setAttribute('aria-expanded', String(isOpen));
    });
    sidebar.querySelectorAll('.sidebar-link').forEach((link) => {
      link.addEventListener('click', () => {
        sidebar.classList.remove('open');
      });
    });
  }

  /* ── Highlight active page ───────────────────── */
  const currentPage = window.location.pathname.split('/').pop() || 'index.html';
  document.querySelectorAll('.sidebar-link').forEach((link) => {
    const href = link.getAttribute('href');
    if (href === currentPage || (currentPage === '' && href === 'index.html')) {
      link.classList.add('active');
    }
  });

  /* ── Smooth scroll for anchor links ──────────── */
  document.querySelectorAll('a[href^="#"]').forEach((anchor) => {
    anchor.addEventListener('click', (e) => {
      const target = document.querySelector(anchor.getAttribute('href'));
      if (target) {
        e.preventDefault();
        target.scrollIntoView({ behavior: 'smooth' });
      }
    });
  });

  /* ── Active section highlighting on scroll ───── */
  const headings = document.querySelectorAll('.docs-content h2[id], .docs-content h3[id]');
  if (headings.length > 0) {
    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((entry) => {
          if (entry.isIntersecting) {
            const id = entry.target.getAttribute('id');
            document.querySelectorAll('.sidebar-link').forEach((link) => {
              link.classList.remove('active-section');
              if (link.getAttribute('href') === '#' + id) {
                link.classList.add('active-section');
              }
            });
          }
        });
      },
      { rootMargin: '-80px 0px -60% 0px', threshold: 0 }
    );
    headings.forEach((h) => observer.observe(h));
  }
});
