// Browser regression test for public/index.html, served by the C server.
// Usage: node tests/page.test.js [path/to/server]   (needs Node + Playwright with Chromium)
// Runs with the CSP and other headers the server sends in production.
'use strict';
const { chromium, devices } = require('playwright');
const { spawn } = require('child_process');
const path = require('path');
const net = require('net');

const REPO = path.resolve(__dirname, '..');
const BIN = path.resolve(process.argv[2] || path.join(REPO, 'build/server'));

const freePort = () => new Promise((res, rej) => {
  const s = net.createServer().listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); });
  s.on('error', rej);
});

async function startServer() {
  const port = await freePort();
  const proc = spawn(BIN, [path.join(REPO, 'public')], { env: { ...process.env, HOST: '127.0.0.1', PORT: String(port) }, stdio: ['ignore', 'ignore', 'pipe'] });
  let log = '';
  proc.stderr.on('data', d => { log += d; });
  const base = `http://127.0.0.1:${port}`;
  for (let i = 0; i < 100; i++) {
    try { if ((await fetch(base + '/healthz')).ok) return { proc, base }; } catch (e) { /* not up yet */ }
    await new Promise(r => setTimeout(r, 50));
  }
  proc.kill();
  throw new Error('server did not start\n' + log);
}

(async () => {
  const { proc, base } = await startServer();
  const proxy = process.env.HTTPS_PROXY ? { server: process.env.HTTPS_PROXY, bypass: '127.0.0.1,localhost' } : undefined;
  const browser = await chromium.launch({ proxy });
  const errors = [];
  const results = [];
  const ok = (name, cond, extra = '') => results.push({ pass: !!cond, line: (cond ? 'PASS ' : 'FAIL ') + name + (extra ? ' — ' + extra : '') });
  const norm = s => s.replace(/[\s  ]+/g, ' ').trim();

  async function newPage(opts = {}, override) {
    const ctx = await browser.newContext({ viewport: { width: 1360, height: 900 }, ignoreHTTPSErrors: true, ...opts });
    const page = await ctx.newPage();
    page.on('console', m => { if (m.type() === 'error') errors.push('console: ' + m.text()); });
    page.on('pageerror', e => errors.push('pageerror: ' + e.message));
    if (override) {
      await page.route(base + '/', async route => {
        const resp = await route.fetch();
        route.fulfill({ response: resp, body: override(await resp.text()) });
      });
    }
    await page.goto(base + '/');
    await page.waitForTimeout(800);
    return { ctx, page };
  }
  const txt = (page, sel) => page.$eval(sel, e => e.textContent).then(norm);
  const setRange = (page, q) => page.$eval('#calcRange', (e, v) => { e.value = v; e.dispatchEvent(new Event('input')); }, q);

  try {
    // ---- default CONFIG ----
    let { ctx, page } = await newPage();
    const ff = i => page.$eval(`#print .ltr:nth-child(${i + 1})`, e => e.style.fontFamily);
    const f0 = await ff(0);
    await page.click('#print .ltr:nth-child(1)');
    const f1 = await ff(0);
    ok('letter click cycles font', f0 !== f1, `${f0} -> ${f1}`);
    ok('variant switched to Savas', await page.$eval('[data-variant="custom"]', e => e.getAttribute('aria-pressed')) === 'true');
    ok('step variant ticked', await page.$eval('[data-step="variant"]', e => e.classList.contains('done')));
    await page.click('[data-variant="B"]'); await page.waitForTimeout(600);
    const bFonts = await page.$$eval('#print .ltr', es => es.map(e => e.style.fontFamily));
    ok('variant B applied', bFonts[0].includes('Abril') && bFonts[1].includes('Bungee'), bFonts.slice(0, 3).join(' | '));
    await page.click('[data-variant="custom"]'); await page.waitForTimeout(600);
    ok('Savas restores custom', (await ff(0)) === f1);
    await page.click('#shuffleBtn');
    await page.click('.config-foot [data-action="add"]');
    await page.waitForTimeout(300);
    const toastTxt = await txt(page, '#toasts');
    ok('no size -> toast', toastTxt.includes('Pirma pasirink dydį'), toastTxt);
    ok('no size -> cart empty', await txt(page, '#cartCount') === '0');
    await page.click('#sizes .size:has-text("M")');
    ok('step size ticked', await page.$eval('[data-step="size"]', e => e.classList.contains('done')));
    ok('benefit size synced', await page.$eval('.demo .size[data-size="M"]', e => e.getAttribute('aria-pressed')) === 'true');
    await page.click('.config-foot [data-action="add"]');
    await page.click('.hero-copy [data-action="add"]');
    ok('cart count 2 (merged)', await txt(page, '#cartCount') === '2');
    ok('step cart ticked', await page.$eval('[data-step="cart"]', e => e.classList.contains('done')));
    await page.click('#cartBtn'); await page.waitForTimeout(500);
    ok('cart opened', await page.$eval('#cart', e => e.classList.contains('open')));
    ok('cart items 1, qty 2', await page.$$eval('#cartItems .item', e => e.length) === 1 && await txt(page, '#cartItems output') === '2');
    const cart2 = [await txt(page, '#cartSub'), await txt(page, '#cartDisc'), await txt(page, '#cartTotal')].join(' | ');
    ok('cart 2 pcs: 10 % discount', cart2 === '45,98 € | −4,60 € (10 %) | 41,38 €', cart2);
    await page.click('#cartItems [data-q="1"]');
    ok('qty + works', await txt(page, '#cartItems output') === '3');
    await page.click('#cartItems [data-q="-1"]');
    ok('qty - works', await txt(page, '#cartItems output') === '2');
    await page.click('#checkoutBtn'); await page.waitForTimeout(300);
    ok('checkout fallback toast (no checkoutUrl/email)', (await txt(page, '#toasts')).includes('Užsakymai atsidarys netrukus'));
    await page.keyboard.press('Escape'); await page.waitForTimeout(500);
    ok('ESC closes cart', !(await page.$eval('#cart', e => e.classList.contains('open'))));
    await setRange(page, 5);
    const calc5 = [await txt(page, '#calcQtyBig'), await txt(page, '#calcDisc'), await txt(page, '#calcSum')].join(' | ');
    ok('calc 5 pcs: 20 % discount', calc5 === '5 | −20 % | 91,96 €', calc5);

    await page.locator('#stage').scrollIntoViewIfNeeded();
    const before = await page.$eval('#annoLines line', l => l.getAttribute('x1') + ',' + l.getAttribute('y1'));
    const box = await page.locator('.anno[data-anno="letter"]').boundingBox();
    const fontsBefore = await page.$$eval('#print .ltr', es => es.map(e => e.style.fontFamily).join());
    await page.mouse.move(box.x + 20, box.y + 10); await page.mouse.down();
    await page.mouse.move(box.x + 60, box.y + 80, { steps: 8 }); await page.mouse.up();
    await page.waitForTimeout(100);
    const after = await page.$eval('#annoLines line', l => l.getAttribute('x1') + ',' + l.getAttribute('y1'));
    const fontsAfterDrag = await page.$$eval('#print .ltr', es => es.map(e => e.style.fontFamily).join());
    ok('drag moves line', before !== after, before + ' -> ' + after);
    ok('drag does not trigger action', fontsBefore === fontsAfterDrag);
    await page.click('.anno[data-anno="letter"]');
    const fontsAfterClick = await page.$$eval('#print .ltr', es => es.map(e => e.style.fontFamily).join());
    ok('anno click changes a letter', fontsAfterClick !== fontsAfterDrag);

    await page.click('#themeBtn');
    ok('theme light', await page.$eval('html', e => e.dataset.theme) === 'light');
    await page.reload(); await page.waitForTimeout(800);
    ok('persist theme+cart+size', await page.$eval('html', e => e.dataset.theme) === 'light' && await txt(page, '#cartCount') === '2' && await page.$eval('#sizes .size[data-size="M"]', e => e.getAttribute('aria-pressed')) === 'true');
    ok('product fonts loaded (Google Fonts allowed by CSP)', await page.evaluate(() => document.fonts.check("20px 'Bungee'") && document.fonts.check("20px 'Pacifico'")));
    await ctx.close();

    // ---- CONFIG with email: mailto checkout ----
    ({ ctx, page } = await newPage({}, h => {
      if (!h.includes('email: null,')) throw new Error('override: email line not found');
      return h.replace('email: null,', "email: 'test@example.com',");
    }));
    await page.evaluate(() => localStorage.clear()); await page.reload(); await page.waitForTimeout(600);
    await setRange(page, 3);
    const calc3 = [await txt(page, '#calcUnit'), await txt(page, '#calcDisc'), await txt(page, '#calcSave'), await txt(page, '#calcSum')].join(' | ');
    ok('calc 3 pcs: 20 % discount', calc3 === '18,39 € | −20 % | 13,79 € | 55,18 €', calc3);
    await page.click('#sizes .size:has-text("L")');
    await setRange(page, 5);
    await page.click('#calcAdd');
    await page.click('#cartBtn'); await page.waitForTimeout(400);
    const cart5 = [await txt(page, '#cartSub'), await txt(page, '#cartDisc'), await txt(page, '#cartTotal')].join(' | ');
    ok('cart 5 pcs from calculator', cart5 === '114,95 € | −22,99 € (20 %) | 91,96 €', cart5);
    const nav = page.waitForEvent('framenavigated', { timeout: 1500 }).catch(() => null);
    await page.click('#checkoutBtn'); await nav; await page.waitForTimeout(300);
    ok('email checkout -> step order', await page.$eval('[data-step="order"]', e => e.classList.contains('done')).catch(() => false));
    ok('progress 3/4 (variant not chosen)', await page.$eval('#progressNum', e => e.textContent).catch(() => '?') === '3');
    await ctx.close();

    // ---- mobile ----
    ({ ctx, page } = await newPage({ ...devices['iPhone 13'] }));
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    ok('mobile no horizontal overflow', overflow <= 0, 'overflow ' + overflow);
    const small = await page.$$eval('button, a.btn, .nav a', es => es.filter(e => { const r = e.getBoundingClientRect(); return r.width > 0 && (r.height < 44 || r.width < 44) && !e.classList.contains('anno') && !e.closest('.print'); }).map(e => (e.className || e.tagName) + ':' + Math.round(e.getBoundingClientRect().width) + 'x' + Math.round(e.getBoundingClientRect().height)));
    ok('mobile tap targets >= 44 px', small.length === 0, small.slice(0, 8).join(', '));
    await page.click('#menuBtn');
    ok('mobile menu opens', await page.$eval('.nav', e => getComputedStyle(e).display) === 'block');
    await page.click('#nav a[href="#kaina"]'); await page.waitForTimeout(800);
    ok('menu closes on link', await page.$eval('.nav', e => getComputedStyle(e).display) === 'none');
    await ctx.close();

    // ---- reduced motion ----
    ({ ctx, page } = await newPage({ reducedMotion: 'reduce' }));
    await page.locator('#stats').scrollIntoViewIfNeeded(); await page.waitForTimeout(300);
    ok('reduced motion: counters final', (await page.$eval('#stats [data-count]', e => e.textContent)) === '10');
    ok('reduced motion: marquee static', await page.$eval('.marquee-track', e => getComputedStyle(e).animationName) === 'none');
    await ctx.close();
  } finally {
    await browser.close();
    proc.kill();
  }

  results.forEach(r => console.log(r.line));
  const failed = results.filter(r => !r.pass).length;
  console.log('console errors:', errors.length ? '\n' + errors.join('\n') : 'none');
  console.log(`----\npage tests: ${results.length - failed} passed, ${failed} failed`);
  process.exit(failed || errors.length ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
