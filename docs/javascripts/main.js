/*
 * Rayforce docs — nav behaviors.
 *
 * Subset of website/script.js scoped to what the doc pages need:
 *   - Mobile nav toggle (hamburger button)
 *   - Sticky-nav scroll shadow
 *   - GitHub star/fork counter (live, with 1h localStorage cache)
 *
 * Page-specific marketing animations (scroll fade-in, terminal demo) live in
 * the marketing pages' bundle only.
 */
document.addEventListener('DOMContentLoaded', () => {
  'use strict';

  // ── Mobile nav toggle ─────────────────────────
  const navToggle = document.querySelector('.nav-toggle');
  const navLinks = document.querySelector('.nav-links');
  if (navToggle && navLinks) {
    navToggle.addEventListener('click', () => {
      const isOpen = navLinks.classList.toggle('open');
      navToggle.classList.toggle('open');
      navToggle.setAttribute('aria-expanded', String(isOpen));
    });
    navLinks.querySelectorAll('a').forEach((link) => {
      link.addEventListener('click', () => {
        navLinks.classList.remove('open');
        navToggle.classList.remove('open');
        navToggle.setAttribute('aria-expanded', 'false');
      });
    });
  }

  // ── Nav scroll shadow ─────────────────────────
  const nav = document.querySelector('.nav');
  if (nav) {
    const updateNav = () => nav.classList.toggle('nav-scrolled', window.scrollY > 50);
    window.addEventListener('scroll', updateNav, { passive: true });
    updateNav();
  }

  // ── Release banner: pill-nav offset + dismiss ─
  // The floating pill nav (landing/about) is position:fixed and would overlap
  // the in-flow banner. rfUpdateBannerOffset() publishes the still-visible
  // banner height as --rf-banner-h so the nav sits just below it and rises back
  // as it scrolls away. Dismissal is keyed to the release tag (set on the
  // element by the resolver) so a new release re-shows the banner.
  const rfBanner = document.querySelector('[data-rf-banner]');
  if (rfBanner) {
    rfUpdateBannerOffset();
    window.addEventListener('scroll', rfUpdateBannerOffset, { passive: true });
    window.addEventListener('resize', rfUpdateBannerOffset, { passive: true });
    const close = rfBanner.querySelector('[data-rf-dismiss]');
    if (close) close.addEventListener('click', () => {
      const tag = rfBanner.dataset.tag;
      if (tag) { try { localStorage.setItem('rf-banner-dismissed', tag); } catch (e) {} }
      rfBanner.hidden = true;
      rfUpdateBannerOffset();
    });
  }
});

/* Publish the still-visible release-banner height as --rf-banner-h (0 when the
 * banner is hidden/absent or fully scrolled past). Pure — safe to call anywhere. */
function rfUpdateBannerOffset() {
  const banner = document.querySelector('[data-rf-banner]');
  const visible = (banner && !banner.hidden)
    ? Math.max(0, banner.offsetHeight - (window.scrollY || 0))
    : 0;
  document.documentElement.style.setProperty('--rf-banner-h', visible + 'px');
}

/* GitHub widget: live stars/forks from the authoritative GitHub API, with
 * ungh.cc (a CDN-cached proxy) as a fallback.  Uses a stale-while-revalidate
 * localStorage cache: a cached value paints instantly, then the count is
 * revalidated in the background (at most once every few minutes, to stay
 * under GitHub's rate limit) and repainted only if it changed — so a stale
 * cache self-corrects on the next visit instead of freezing.  Falls back
 * silently if every source is unreachable. */
(function () {
  const targets = document.querySelectorAll('[data-gh-stat]');
  if (targets.length === 0) return;

  const REPO = 'RayforceDB/rayforce';
  const CACHE_KEY = 'gh-stats:' + REPO;
  const REVALIDATE_AFTER = 5 * 60 * 1000; /* skip the background refetch if the cache is younger than this */

  function fmt(n) {
    if (typeof n !== 'number' || isNaN(n)) return '—';
    if (n >= 10000) return (n / 1000).toFixed(1).replace(/\.0$/, '') + 'k';
    if (n >= 1000)  return (n / 1000).toFixed(1) + 'k';
    return String(n);
  }

  function animateCount(el, target, duration) {
    if (typeof target !== 'number' || isNaN(target)) return;
    const reduce = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    if (reduce) { el.textContent = fmt(target); return; }
    const start = performance.now();
    function tick(now) {
      const t = Math.min(1, (now - start) / duration);
      const eased = 1 - Math.pow(1 - t, 3); /* easeOutCubic */
      el.textContent = fmt(Math.round(target * eased));
      if (t < 1) requestAnimationFrame(tick);
      else el.textContent = fmt(target);
    }
    requestAnimationFrame(tick);
  }

  function paint(stars, forks) {
    targets.forEach(el => {
      const which = el.getAttribute('data-gh-stat');
      const target = which === 'stars' ? stars
                   : which === 'forks' ? forks
                   : NaN;
      animateCount(el, target, 900);
    });
  }

  function readCache() {
    try {
      const v = JSON.parse(localStorage.getItem(CACHE_KEY));
      return (v && typeof v.stars === 'number' && typeof v.forks === 'number') ? v : null;
    } catch (e) { return null; }
  }

  function writeCache(stars, forks) {
    try { localStorage.setItem(CACHE_KEY, JSON.stringify({ t: Date.now(), stars, forks })); } catch (e) {}
  }

  /* Persist the latest counts, and repaint only when they actually changed —
   * avoids re-animating an identical value the cache already showed. */
  function update(stars, forks) {
    const changed = !cached || cached.stars !== stars || cached.forks !== forks;
    writeCache(stars, forks);
    if (changed) paint(stars, forks);
  }

  /* Paint the cached value instantly (may be slightly stale), then revalidate. */
  const cached = readCache();
  if (cached) paint(cached.stars, cached.forks);
  if (cached && (Date.now() - cached.t) < REVALIDATE_AFTER) return; /* fresh enough; skip the network */

  /* GitHub's own API is authoritative and fresh; the per-visitor 60/hour
   * unauthenticated limit is ample for one request every few minutes.
   * ungh.cc is a CDN-cached proxy that can lag GitHub by ~a day, so it is
   * used only as a fallback when the direct API is unreachable. */
  fetch('https://api.github.com/repos/' + REPO, { headers: { Accept: 'application/vnd.github+json' } })
    .then(r => r.ok ? r.json() : Promise.reject(new Error('gh ' + r.status)))
    .then(d => {
      const stars = d && d.stargazers_count;
      const forks = d && d.forks_count;
      if (typeof stars !== 'number' || typeof forks !== 'number') throw new Error('gh shape');
      update(stars, forks);
    })
    .catch(() => {
      return fetch('https://ungh.cc/repos/' + REPO)
        .then(r => r.ok ? r.json() : Promise.reject(new Error('ungh ' + r.status)))
        .then(d => {
          const stars = d && d.repo && d.repo.stars;
          const forks = d && d.repo && d.repo.forks;
          if (typeof stars !== 'number' || typeof forks !== 'number') throw new Error('ungh shape');
          update(stars, forks);
        });
    })
    .catch(() => { /* leave whatever is painted (cache or em-dash placeholders) */ });
})();

/* Latest-release resolver: powers the OS-aware Download CTAs (landing) and the
 * site-wide release announcement banner from a single fetch. ungh.cc primary
 * (CDN-cached, no rate limit), GitHub API fallback, 1h localStorage cache.
 * ungh leaves asset `name` null, so we match on the download URL, which carries
 * the platform token (linux-x86_64 / darwin-arm64). Progressive enhancement:
 * the CTAs already link to /releases/latest and the banner already names the
 * project, so it all works with no JS, on Windows/mobile, or if APIs are down. */
(function () {
  const ctas = document.querySelectorAll('[data-download-cta]');
  const tagEls = document.querySelectorAll('[data-release-tag]');
  const metaEl = document.querySelector('[data-download-meta]');
  if (ctas.length === 0 && tagEls.length === 0) return;

  const REPO = 'RayforceDB/rayforce';
  const CACHE_KEY = 'gh-release:' + REPO;
  const CACHE_TTL = 60 * 60 * 1000; /* 1 hour */

  /* Only Linux x86_64 and macOS (arm64) ship today; Windows/mobile/unknown fall
   * through to the releases page. navigator.platform can't tell Apple Silicon
   * from Intel, but arm64 is the only macOS artifact, so 'darwin' is correct. */
  function detectTarget() {
    const s = (navigator.userAgent || '') + ' ' + (navigator.platform || '');
    if (/Android/i.test(s)) return null;
    if (/Mac/i.test(s))     return { token: 'darwin', os: 'macOS' };
    if (/Linux|X11/i.test(s)) return { token: 'linux', os: 'Linux' };
    return null;
  }

  function apply(rel) {
    if (!rel || !rel.tag) return;
    const tgt = detectTarget();
    const assetUrl = tgt
      ? (rel.urls || []).find(u => /\.tar\.gz$/.test(u) && u.indexOf(tgt.token) !== -1)
      : null;

    ctas.forEach(a => {
      const label = a.querySelector('[data-download-label]');
      if (assetUrl) {
        a.href = assetUrl;
        if (label) label.textContent = 'Download for ' + tgt.os;
        a.setAttribute('title', 'Rayforce ' + rel.tag + ' · ' + tgt.os);
      } else if (label) {
        label.textContent = 'Download';   /* keep the /releases/latest page href */
      }
    });

    tagEls.forEach(el => { el.textContent = 'Rayforce ' + rel.tag + ' is out'; });

    /* Show the banner unless it was dismissed for THIS exact release tag — a
     * newer release re-shows it. The tag is stashed on the element for the
     * dismiss handler. */
    const banner = document.querySelector('[data-rf-banner]');
    if (banner) {
      banner.dataset.tag = rel.tag;
      let dismissed = null;
      try { dismissed = localStorage.getItem('rf-banner-dismissed'); } catch (e) {}
      banner.hidden = (dismissed === rel.tag);
      if (typeof rfUpdateBannerOffset === 'function') rfUpdateBannerOffset();
    }

    if (metaEl) {
      metaEl.textContent = assetUrl
        ? 'Latest ' + rel.tag + ' · ' + tgt.os
        : 'Latest release: ' + rel.tag;
      metaEl.hidden = false;
    }
  }

  function readCache() {
    try {
      const v = JSON.parse(localStorage.getItem(CACHE_KEY));
      if (!v || (Date.now() - v.t) > CACHE_TTL) return null;
      return v.rel;
    } catch (e) { return null; }
  }
  function writeCache(rel) {
    try { localStorage.setItem(CACHE_KEY, JSON.stringify({ t: Date.now(), rel: rel })); } catch (e) {}
  }

  const cached = readCache();
  if (cached) { apply(cached); return; }

  /* Primary: ungh.cc. Shape: { release: { tag, assets: [{ downloadUrl }] } }. */
  fetch('https://ungh.cc/repos/' + REPO + '/releases/latest')
    .then(r => r.ok ? r.json() : Promise.reject(new Error('ungh ' + r.status)))
    .then(d => {
      const r = d.release || d;
      const rel = { tag: r.tag, urls: (r.assets || []).map(a => a.downloadUrl).filter(Boolean) };
      if (!rel.tag) throw new Error('ungh shape');
      writeCache(rel); apply(rel);
    })
    .catch(() => {
      /* Fallback: GitHub API. Shape: { tag_name, assets: [{ browser_download_url }] }. */
      return fetch('https://api.github.com/repos/' + REPO + '/releases/latest', { headers: { Accept: 'application/vnd.github+json' } })
        .then(r => r.ok ? r.json() : Promise.reject(new Error('gh ' + r.status)))
        .then(d => {
          const rel = { tag: d.tag_name, urls: (d.assets || []).map(a => a.browser_download_url).filter(Boolean) };
          if (!rel.tag) throw new Error('gh shape');
          writeCache(rel); apply(rel);
        });
    })
    .catch(() => { /* CTAs stay on /releases/latest; banner keeps its default text. */ });
})();
