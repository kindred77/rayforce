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
});

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
