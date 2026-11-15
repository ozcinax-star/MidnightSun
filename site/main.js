/* MidnightSun site: reveals, the journey scroll scene, the phone download dock. */
(function () {
  'use strict';
  var root = document.documentElement;
  var reduce = window.matchMedia('(prefers-reduced-motion: reduce)');

  /* 1. reveal once, never replay */
  var revealEls = document.querySelectorAll('[data-reveal]');
  if ('IntersectionObserver' in window) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) {
        if (e.isIntersecting) { e.target.classList.add('is-in'); io.unobserve(e.target); }
      });
    }, { rootMargin: '0px 0px -10% 0px' });
    revealEls.forEach(function (el, i) {
      el.style.setProperty('--i', String(i % 3));
      io.observe(el);
    });
  } else {
    revealEls.forEach(function (el) { el.classList.add('is-in'); });
  }

  /* 2. journey: the pinned section steps through five states as you scroll */
  var journey = document.querySelector('.journey-track');
  var viz = document.querySelector('.viz');
  var stageItems = document.querySelectorAll('.stages li');
  var pinned = window.matchMedia('(min-width: 900px) and (prefers-reduced-motion: no-preference)');
  var current = 0;

  function setStage(n) {
    if (n === current) return;
    current = n;
    viz.setAttribute('data-stage', String(n));
    stageItems.forEach(function (li) {
      li.classList.toggle('is-on', Number(li.getAttribute('data-stage')) === n);
    });
  }

  function onScroll() {
    if (!pinned.matches) return;
    var r = journey.getBoundingClientRect();
    var travel = journey.offsetHeight - window.innerHeight;
    var p = travel > 0 ? Math.min(1, Math.max(0, -r.top / travel)) : 0;
    setStage(Math.min(5, Math.floor(p * 5) + 1));
  }

  function applyMode() {
    if (pinned.matches) {
      current = 0;
      onScroll();
    } else {
      current = 0;
      viz.setAttribute('data-stage', 'all');
      stageItems.forEach(function (li) { li.classList.remove('is-on'); });
    }
  }

  var ticking = false;
  window.addEventListener('scroll', function () {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(function () { ticking = false; onScroll(); updateDock(); });
  }, { passive: true });
  if (pinned.addEventListener) pinned.addEventListener('change', applyMode);
  window.addEventListener('resize', onScroll);

  /* 3. phone dock: show after the hero, hide over the download section */
  var dock = document.querySelector('.dock');
  var hero = document.querySelector('.hero');
  var dl = document.getElementById('indir');
  function updateDock() {
    if (!dock) return;
    var heroGone = hero.getBoundingClientRect().bottom < 0;
    var d = dl.getBoundingClientRect();
    var overDl = d.top < window.innerHeight && d.bottom > 0;
    var show = heroGone && !overDl;
    dock.classList.toggle('is-shown', show);
    if (show) { dock.removeAttribute('tabindex'); dock.removeAttribute('aria-hidden'); }
    else { dock.setAttribute('tabindex', '-1'); dock.setAttribute('aria-hidden', 'true'); }
  }

  applyMode();
  updateDock();
  root.classList.add('motion-ready');
})();
