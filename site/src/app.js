// Three behaviours: trace a language across a chart, show data-tip tooltips,
// and switch the variance chart's metric. Everything is readable without JS.

const tip = document.querySelector('.tip');

function showTip(el, x, y) {
  tip.textContent = el.dataset.tip;
  tip.hidden = false;
  const pad = 14;
  const { width, height } = tip.getBoundingClientRect();
  const left = x + pad + width > innerWidth ? x - pad - width : x + pad;
  const top = y + pad + height > innerHeight ? y - pad - height : y + pad;
  tip.style.left = `${Math.max(4, left)}px`;
  tip.style.top = `${Math.max(4, top)}px`;
}

document.addEventListener('pointerover', (e) => {
  const el = e.target.closest('[data-tip]');
  if (el) showTip(el, e.clientX, e.clientY);
});
document.addEventListener('pointermove', (e) => {
  const el = e.target.closest('[data-tip]');
  if (el) showTip(el, e.clientX, e.clientY);
  else tip.hidden = true;
});
document.addEventListener('pointerleave', () => { tip.hidden = true; });
addEventListener('scroll', () => { tip.hidden = true; }, { passive: true });

// Hover traces a language; a click (or tap, or Enter on a label) pins it.
for (const chart of document.querySelectorAll('[data-highlight]')) {
  let pinned = null;
  const marks = chart.querySelectorAll('[data-lang]');
  const focus = (lang) => {
    chart.classList.toggle('is-focused', Boolean(lang));
    for (const m of marks) m.classList.toggle('is-on', m.dataset.lang === lang);
  };
  chart.addEventListener('pointerover', (e) => {
    const el = e.target.closest('[data-lang]');
    if (el && !pinned) focus(el.dataset.lang);
  });
  chart.addEventListener('pointerleave', () => focus(pinned));
  chart.addEventListener('click', (e) => {
    const el = e.target.closest('[data-lang]');
    pinned = el && el.dataset.lang !== pinned ? el.dataset.lang : null;
    focus(pinned);
  });
  chart.addEventListener('focusin', (e) => {
    const el = e.target.closest('[data-lang]');
    if (el && !pinned) focus(el.dataset.lang);
  });
  chart.addEventListener('focusout', () => focus(pinned));
}

for (const seg of document.querySelectorAll('.seg')) {
  const buttons = seg.querySelectorAll('button');
  for (const button of buttons) {
    button.addEventListener('click', () => {
      for (const b of buttons) {
        const on = b === button;
        b.setAttribute('aria-selected', String(on));
        document.getElementById(b.dataset.show).hidden = !on;
      }
    });
  }
}
