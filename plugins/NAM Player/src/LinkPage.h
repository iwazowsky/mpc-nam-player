// LinkPage.h - the web page the live link serves (see LinkServer.h).
//
// One self-contained HTML file. It runs in the browser of the computer or phone
// connected to the MPC: that browser fetches profiles from the internet and
// sends them to the plugin.
//
// Layout: a console that never leaves the screen (what is playing, levels, the
// four sound controls) beside one pane of tabs (the card, TONE3000, the community
// collection, your own files). Set-and-forget things live behind the gear.

#ifndef NAMPLAYER_LINK_PAGE_H
#define NAMPLAYER_LINK_PAGE_H

namespace namplayer {

inline const char* const kLinkPage = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>NAM Player</title>
<style>
  :root { color-scheme: light dark;
    --bg:#eceff0; --surface:#f7f9f9; --raised:#ffffff; --hover:rgba(28,35,39,.06);
    --ink:#1c2327; --muted:#63707a; --line:#d7dde0; --track:#d0d7db; --thumb:#ffffff;
    --accent:#1d7868; --accent-soft:rgba(29,120,104,.11); --on-accent:#ffffff;
    --bad:#b3372c; --bad-soft:rgba(179,55,44,.10); --shade:rgba(15,25,30,.10); }
  @media (prefers-color-scheme: dark) { :root {
    --bg:#14181b; --surface:#1b2125; --raised:#262d33; --hover:rgba(221,227,230,.07);
    --ink:#dce2e5; --muted:#8a969d; --line:#2c353b; --track:#343e45; --thumb:#e4eaec;
    --accent:#66bfa9; --accent-soft:rgba(102,191,169,.14); --on-accent:#0b1f1a;
    --bad:#ee8f82; --bad-soft:rgba(238,143,130,.13); --shade:rgba(0,0,0,.35); } }
  * { box-sizing: border-box; }
  [hidden] { display: none !important; }
  html { -webkit-text-size-adjust: 100%; }
  body { margin: 0; background: var(--bg); color: var(--ink); font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
  button, input, select { font: inherit; color: inherit; }
  button { cursor: pointer; border: 0; background: none; padding: 0; -webkit-tap-highlight-color: transparent; }
  button:disabled { opacity: .4; cursor: default; }
  :focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
  p { margin: 0; } a { color: var(--accent); } code { font-size: .92em; overflow-wrap: anywhere; }
  .muted { color: var(--muted); } .small { font-size: 13px; }
  svg { width: 18px; height: 18px; fill: none; stroke: currentColor; stroke-width: 2; stroke-linecap: round; stroke-linejoin: round; flex: none; }

  .app { max-width: 1060px; margin: 0 auto; padding: 0 max(16px, env(safe-area-inset-right)) 90px max(16px, env(safe-area-inset-left)); display: grid; grid-template-columns: 330px minmax(0, 1fr); gap: 28px; align-items: start; }
  .console { position: sticky; top: 20px; margin-top: 20px; background: var(--surface); border-radius: 18px; padding: 16px 18px 8px; display: grid; gap: 14px; }
  .pane { padding-top: 20px; min-width: 0; }

  /* the deck: what is playing, and the levels */
  .brand { display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--muted); }
  .brand b { color: var(--ink); font-weight: 600; } .brand .sp { flex: 1; }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--accent); } .dot.off { background: var(--bad); }
  .icon { width: 36px; height: 36px; border-radius: 10px; display: grid; place-items: center; color: var(--muted); flex: none; }
  .icon:hover:not(:disabled) { background: var(--hover); color: var(--ink); }
  .now { display: flex; align-items: center; gap: 4px; margin-top: 6px; }
  .now .what { flex: 1 1 auto; min-width: 0; }
  #name { font-size: 19px; font-weight: 650; line-height: 1.25; overflow-wrap: anywhere; }
  #sub { font-size: 13px; color: var(--muted); }
  .flag { font-size: 13px; padding: 7px 10px; border-radius: 9px; margin-top: 10px; background: var(--hover); color: var(--muted); }
  .flag.bad { background: var(--bad-soft); color: var(--bad); }
  .meters { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 14px; margin-top: 12px; }
  .m .lab { display: flex; justify-content: space-between; align-items: baseline; font-size: 11px; color: var(--muted); letter-spacing: .07em; text-transform: uppercase; }
  .m .lab b { font-weight: 500; font-size: 12px; color: var(--ink); letter-spacing: 0; text-transform: none; font-variant-numeric: tabular-nums; }
  .bar { height: 4px; border-radius: 2px; background: var(--track); overflow: hidden; margin-top: 5px; }
  .bar i { display: block; height: 100%; width: 0; background: var(--accent); border-radius: 2px; transition: width .9s linear; }
  .bar.hot i { background: var(--bad); }

  /* the four sound controls */
  .sound { border-top: 1px solid var(--line); }
  .fold { display: flex; width: 100%; align-items: center; gap: 8px; min-height: 42px; text-align: left; font-weight: 600; }
  .fold .sum { flex: 1; min-width: 0; font-weight: 400; color: var(--muted); font-size: 13px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; text-align: right; }
  .fold svg { width: 16px; height: 16px; color: var(--muted); transition: transform .15s; }
  #ctl { display: grid; gap: 12px; padding: 2px 0 12px; }
  .ctl { display: grid; grid-template-columns: 1fr auto; gap: 0 10px; align-items: baseline; }
  .ctl label { font-size: 13px; color: var(--muted); }
  .ctl output { font-size: 13px; font-weight: 600; font-variant-numeric: tabular-nums; }
  .ctl input { grid-column: 1 / -1; }
  input[type=range] { -webkit-appearance: none; appearance: none; width: 100%; height: 30px; margin: 0; background: transparent; cursor: pointer; --p: 50%; }
  input[type=range]::-webkit-slider-runnable-track { height: 6px; border-radius: 3px; background: linear-gradient(var(--accent), var(--accent)) 0 0 / var(--p) 100% no-repeat var(--track); }
  input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 20px; height: 20px; margin-top: -7px; border-radius: 50%; background: var(--thumb); box-shadow: 0 0 0 1px var(--line), 0 1px 3px var(--shade); }
  input[type=range]::-moz-range-track { height: 6px; border-radius: 3px; background: var(--track); }
  input[type=range]::-moz-range-progress { height: 6px; border-radius: 3px; background: var(--accent); }
  input[type=range]::-moz-range-thumb { width: 20px; height: 20px; border: 0; border-radius: 50%; background: var(--thumb); box-shadow: 0 0 0 1px var(--line), 0 1px 3px var(--shade); }

  /* the pane */
  .tabs { display: flex; gap: 4px; background: var(--surface); padding: 4px; border-radius: 13px; overflow-x: auto; scrollbar-width: none; }
  .tabs::-webkit-scrollbar { display: none; }
  .tabs button { flex: 1 0 auto; min-height: 38px; padding: 6px 12px; border-radius: 9px; color: var(--muted); font-weight: 500; white-space: nowrap; }
  .tabs button:hover { color: var(--ink); }
  .tabs button[aria-selected=true], .seg button.on { background: var(--raised); color: var(--ink); box-shadow: 0 1px 2px var(--shade); }
  .view { padding-top: 16px; display: grid; gap: 12px; } .view:empty { display: none; }
  .note { margin-top: 12px; }
  .field { width: 100%; min-width: 0; min-height: 40px; padding: 8px 12px; border-radius: 10px; border: 1px solid var(--line); background: var(--raised); }
  .field::placeholder { color: var(--muted); }
  select.field { width: auto; }
  .btn { display: inline-flex; align-items: center; justify-content: center; cursor: pointer; min-height: 40px; padding: 8px 14px; border-radius: 10px; background: var(--surface); font-weight: 500; white-space: nowrap; }
  .btn:hover:not(:disabled) { background: var(--raised); }
  .btn.soft { background: var(--accent-soft); color: var(--accent); font-weight: 600; } .btn.soft:hover:not(:disabled) { background: var(--accent-soft); filter: brightness(1.15); }
  .btn.primary { background: var(--accent); color: var(--on-accent); font-weight: 600; } .btn.primary:hover:not(:disabled) { background: var(--accent); filter: brightness(1.08); }
  .line { display: flex; gap: 8px; align-items: center; } .line > .grow { flex: 1 1 auto; min-width: 0; } .line.wrap { flex-wrap: wrap; }
  .line label { white-space: nowrap; }
  .card { background: var(--surface); border-radius: 14px; padding: 4px 14px; } .card.pad { padding: 14px; display: grid; gap: 12px; }
  .empty { padding: 28px 8px; text-align: center; color: var(--muted); }
  .linkish { color: var(--accent); text-decoration: underline; }

  /* the card's library */
  #lib { display: grid; gap: 10px; }
  .group { border-radius: 14px; background: var(--surface); overflow: hidden; }
  .ghead { display: flex; width: 100%; align-items: center; gap: 10px; padding: 12px 14px; text-align: left; font-weight: 600; }
  .ghead .t { flex: 1 1 auto; min-width: 0; overflow-wrap: anywhere; }
  .ghead .n { color: var(--muted); font-weight: 400; font-size: 13px; font-variant-numeric: tabular-nums; }
  .ghead svg { width: 16px; height: 16px; color: var(--muted); transition: transform .15s; }
  .group.shut .ghead svg, .sound.shut .fold svg { transform: rotate(-90deg); }
  .rows { padding: 0 6px 6px; } .group.shut .rows { display: none; }
  .row { display: flex; align-items: center; border-radius: 10px; }
  .row:hover { background: var(--hover); } .row.now { background: var(--accent-soft); }
  .row .play { flex: 1 1 auto; min-width: 0; display: flex; align-items: baseline; gap: 10px; padding: 9px 8px; min-height: 42px; text-align: left; }
  .row .i { width: 1.7em; text-align: right; color: var(--muted); font-size: 12px; font-variant-numeric: tabular-nums; flex: none; }
  .row .t { flex: 1 1 auto; min-width: 0; overflow-wrap: anywhere; } .row.now .t { color: var(--accent); font-weight: 600; }
  .row .k { color: var(--muted); font-size: 12px; white-space: nowrap; flex: none; }
  .row .del { display: none; } .editing .row .del { display: grid; }
  .row .del.sure { width: auto; padding: 0 10px; color: var(--bad); font-size: 13px; font-weight: 600; }

  /* rows that send a profile to the card */
  .srow { display: grid; grid-template-columns: minmax(0, 1fr) 172px auto; gap: 8px; align-items: center; padding: 9px 0; border-top: 1px solid var(--line); }
  .srow:first-child { border-top: 0; }
  .srow .t { font-size: 13px; overflow-wrap: anywhere; } .srow .field { font-size: 14px; }
  .drop { border: 1.5px dashed var(--line); border-radius: 14px; padding: 30px 16px; display: grid; gap: 10px; justify-items: center; text-align: center; color: var(--muted); }
  .drop.over { border-color: var(--accent); color: var(--accent); background: var(--accent-soft); }
  details summary { cursor: pointer; } details > div { display: grid; gap: 10px; padding-top: 8px; justify-items: start; }

  /* settings */
  dialog { border: 0; border-radius: 18px; padding: 0; background: var(--surface); color: var(--ink); width: min(440px, calc(100vw - 24px)); max-height: calc(100vh - 24px); box-shadow: 0 12px 40px var(--shade); }
  dialog:focus { outline: none; }
  dialog::backdrop { background: rgba(10, 14, 16, .5); }
  .dlg { padding: 16px 18px 20px; display: grid; gap: 20px; }
  .dhead { display: flex; align-items: center; } .dhead h2 { margin: 0; font-size: 16px; flex: 1; }
  .set { display: grid; gap: 8px; } .set h3 { margin: 0; font-size: 14px; font-weight: 600; } .set p { font-size: 13px; color: var(--muted); }
  .seg { display: flex; gap: 4px; background: var(--bg); padding: 4px; border-radius: 12px; }
  .seg button { flex: 1; min-height: 36px; border-radius: 8px; color: var(--muted); font-weight: 500; }
  .sw { width: 44px; height: 26px; border-radius: 13px; background: var(--track); position: relative; flex: none; transition: background .15s; }
  .sw::after { content: ""; position: absolute; top: 3px; left: 3px; width: 20px; height: 20px; border-radius: 50%; background: var(--thumb); box-shadow: 0 1px 2px var(--shade); transition: transform .15s; }
  .sw[aria-checked=true] { background: var(--accent); } .sw[aria-checked=true]::after { transform: translateX(18px); }
  dl { display: grid; grid-template-columns: auto minmax(0, 1fr); gap: 5px 14px; margin: 0; font-size: 13px; }
  dt { color: var(--muted); } dd { margin: 0; font-variant-numeric: tabular-nums; }
  #toast { position: fixed; left: 50%; bottom: calc(18px + env(safe-area-inset-bottom)); transform: translateX(-50%); background: var(--ink); color: var(--bg); padding: 10px 16px; border-radius: 12px; max-width: 92vw; font-size: 14px; z-index: 20; box-shadow: 0 6px 20px var(--shade); }
  #toast.bad { background: var(--bad); color: var(--bg); }

  @media (min-width: 821px) { .fold { pointer-events: none; } .fold svg, .fold .sum { display: none; } }
  @media (max-width: 820px) {
    .app { display: block; }
    .console { display: contents; }
    .deck { position: sticky; top: 0; z-index: 5; background: var(--bg); margin: 0 -16px; padding: calc(10px + env(safe-area-inset-top)) 16px 12px; border-bottom: 1px solid var(--line); }
    .sound { border-top: 0; background: var(--surface); border-radius: 14px; padding: 2px 14px; margin-top: 14px; }
    .sound.shut #ctl { display: none; }
    .sound:not(.shut) .fold .sum { visibility: hidden; }
    .pane { padding-top: 14px; }
    .tabs button { padding: 6px; font-size: 14px; }
  }
  @media (max-width: 520px) { .srow { grid-template-columns: minmax(0, 1fr) auto; } .srow .t { grid-column: 1 / -1; } }
  @media (prefers-reduced-motion: reduce) { * { transition: none !important; } }
</style></head><body>
<div class="app">
  <aside class="console">
    <div class="deck">
      <div class="brand"><span class="dot" id="dot"></span><b>NAM Player</b><span id="where"></span><span class="sp"></span>
        <button class="icon" id="gear" title="Settings" aria-label="Settings"><svg viewBox="0 0 24 24"><path d="M4 7h9M17 7h3M4 17h3M11 17h9"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/></svg></button></div>
      <div class="now">
        <div class="what"><div id="name">Connecting...</div><div id="sub"></div></div>
        <button class="icon" id="prev" title="Previous profile" aria-label="Previous profile" disabled><svg viewBox="0 0 24 24"><path d="M15 6l-6 6 6 6"/></svg></button>
        <button class="icon" id="next" title="Next profile" aria-label="Next profile" disabled><svg viewBox="0 0 24 24"><path d="M9 6l6 6-6 6"/></svg></button>
      </div>
      <div class="flag" id="flag" hidden></div>
      <div class="meters">
        <div class="m" id="inM"><div class="lab">In <b id="inTxt"></b></div><div class="bar" id="inBar"><i></i></div></div>
        <div class="m" id="outM"><div class="lab">Out <b id="outTxt"></b></div><div class="bar" id="outBar"><i></i></div></div>
        <div class="m" id="cpuM"><div class="lab">CPU <b id="cpuTxt"></b></div><div class="bar" id="cpuBar"><i></i></div></div>
      </div>
    </div>
    <div class="sound" id="sound">
      <button class="fold" id="fold" aria-expanded="true"><span>Sound</span><span class="sum" id="sum"></span><svg viewBox="0 0 24 24"><path d="M6 9l6 6 6-6"/></svg></button>
      <div id="ctl"></div>
    </div>
  </aside>

  <main class="pane">
    <div class="tabs" role="tablist">
      <button role="tab" data-tab="lib">On the card</button><button role="tab" data-tab="t3k">TONE3000</button><button role="tab" data-tab="com">Community</button><button role="tab" data-tab="up">Upload</button>
    </div>
    <p class="flag bad note" id="nocard" hidden>This copy of the plugin is not installed in "VST2 Plugins" on a card, so there is nowhere to save profiles.</p>

    <section class="view" id="view-lib">
      <div class="line"><input class="field grow" type="search" id="find" placeholder="Search the card" autocomplete="off"><button class="btn" id="edit">Edit</button></div>
      <div id="lib"></div>
    </section>

    <section class="view" id="view-t3k" hidden>
      <div id="t3k" class="view" style="padding-top:0"></div><div id="tone" class="view" style="padding-top:0"></div>
      <p class="small muted">Powered by <a href="https://www.tone3000.com" target="_blank" rel="noopener">TONE3000</a>. This browser downloads only what you send. The card keeps each tone's creator and licence in an ABOUT.txt.</p>
    </section>

    <section class="view" id="view-com" hidden>
      <div class="line"><input class="field grow" type="search" id="q" placeholder="Filter, e.g. fender, 5150, jcm" autocomplete="off"></div>
      <div class="line"><label class="small muted" for="comFolder">Save to folder</label><input class="field grow" type="text" id="comFolder" value="Community" maxlength="24"></div>
      <div class="card" id="cat"></div>
      <p class="small muted" id="catNote"></p>
    </section>

    <section class="view" id="view-up" hidden>
      <div class="line"><label class="small muted" for="folder">Save to folder</label><input class="field grow" type="text" id="folder" value="My Profiles" maxlength="24"></div>
      <div class="drop" id="drop"><span id="dropMsg">Drop .nam files anywhere on this page</span><label class="btn primary" for="pick">Choose files</label><input type="file" id="pick" accept=".nam" multiple hidden></div>
      <p class="small muted">The last file starts playing. Names are cut to 24 characters; the MPC Sample shows about nine.</p>
    </section>
  </main>
</div>

<dialog id="settings" aria-labelledby="setTitle" tabindex="-1"><div class="dlg">
  <div class="dhead"><h2 id="setTitle">Settings</h2><button class="icon" id="setClose" aria-label="Close"><svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg></button></div>
  <div class="set"><h3>Quality</h3><div class="seg" id="quality"><button>Auto</button><button>Lite</button><button>Full</button></div>
    <p>For A2 profiles, which carry a full and a light version. Auto plays the full one and moves to the light one when the CPU runs high.</p></div>
  <div class="set"><div class="line"><h3 class="grow">Normalize</h3><button class="sw" id="norm" role="switch" aria-checked="false" aria-label="Normalize"></button></div>
    <p>Brings every profile to the same loudness, so switching does not jump in volume.</p></div>
  <div class="set"><h3>Right now</h3><dl>
    <dt>Model</dt><dd id="fEng"></dd><dt>Input</dt><dd id="fIn"></dd><dt>Output</dt><dd id="fOut"></dd><dt>CPU</dt><dd id="fCpu"></dd><dt>MPC</dt><dd id="fHost"></dd></dl></div>
  <p class="small muted">Turning the Model or Folder knob on the MPC takes over from this page.</p>
</div></dialog>
<div id="toast" hidden></div>
<script>
const $ = id => document.getElementById(id), H = { 'X-NAM-Player': '1' };
const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const RAW = 'https://raw.githubusercontent.com/pelennor2170/NAM_models/main/';
const CHEV = '<svg viewBox="0 0 24 24"><path d="M6 9l6 6 6-6"/></svg>', BIN = '<svg viewBox="0 0 24 24"><path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/></svg>';
const store = { get(k, d) { try { return localStorage.getItem('nam_' + k) || d; } catch (e) { return d; } }, set(k, v) { try { localStorage.setItem('nam_' + k, v); } catch (e) {} } };
let S = null, catalog = [], names = {}, busy = false, held = {}, timers = {}, flat = [], open = null, lastKey = null, libSig = '', sel = null;

// ---- tabs, settings, the fold
const TABS = ['lib', 't3k', 'com', 'up'];
function show(tab) {
  if (!TABS.includes(tab)) tab = 'lib';
  document.querySelectorAll('.tabs button').forEach(b => b.setAttribute('aria-selected', b.dataset.tab === tab));
  TABS.forEach(t => $('view-' + t).hidden = t !== tab); store.set('tab', tab);
}
document.querySelectorAll('.tabs button').forEach(b => b.onclick = () => show(b.dataset.tab));
show(store.get('tab', 'lib'));
const dlg = $('settings');
$('gear').onclick = () => { dlg.showModal ? dlg.showModal() : dlg.setAttribute('open', ''); dlg.focus(); };
$('setClose').onclick = () => dlg.close ? dlg.close() : dlg.removeAttribute('open');
dlg.onclick = e => { if (e.target === dlg) dlg.close(); };
function fold(shut) { $('sound').classList.toggle('shut', shut); $('fold').setAttribute('aria-expanded', !shut); }
fold(store.get('sound', 'open') === 'shut');
$('fold').onclick = () => { const shut = !$('sound').classList.contains('shut'); fold(shut); store.set('sound', shut ? 'shut' : 'open'); };

function toast(m, bad) { const t = $('toast'); t.textContent = m; t.className = bad ? 'bad' : ''; t.hidden = false; clearTimeout(toast.t); toast.t = setTimeout(() => t.hidden = true, bad ? 7000 : 3000); }
async function call(method, path, body) {
  const r = await fetch(path, { method, headers: H, body });
  const j = await r.json().catch(() => ({ error: 'bad answer from the plugin' }));
  if (!r.ok || j.error) throw new Error(j.error || 'HTTP ' + r.status);
  return j;
}

// ---- sound controls
const CTL = [['drive', 'Drive', ' dB'], ['mix', 'Mix', ' %'], ['tone', 'Tone', ' Hz'], ['level', 'Level', ' dB']];
// Tone is a frequency: the slider moves in equal musical steps, not equal Hz.
const toPos = (id, p) => id === 'tone' ? Math.round(1000 * Math.log(p.value / p.min) / Math.log(p.max / p.min)) : Math.round(1000 * (p.value - p.min) / (p.max - p.min));
const fromPos = (id, p, x) => id === 'tone' ? p.min * Math.pow(p.max / p.min, x / 1000) : p.min + (p.max - p.min) * x / 1000;
const recent = id => Date.now() - (held[id] || 0) < 1500;
function setParam(id, value) { held[id] = Date.now(); clearTimeout(timers[id]); timers[id] = setTimeout(() => call('POST', '/api/param?id=' + id + '&value=' + value).catch(e => toast(e.message, true)), 60); }
function controls() {
  const P = S.params;
  if (!$('ctl').firstChild) {
    $('ctl').innerHTML = CTL.map(([id, name]) => `<div class="ctl"><label for="r_${id}">${name}</label><output id="v_${id}" for="r_${id}"></output><input type="range" min="0" max="1000" id="r_${id}"></div>`).join('');
    CTL.forEach(([id, , unit]) => $('r_' + id).oninput = e => {
      const v = fromPos(id, S.params[id], +e.target.value); e.target.style.setProperty('--p', e.target.value / 10 + '%');
      $('v_' + id).textContent = id === 'tone' ? (v >= 19500 ? 'Off' : Math.round(v) + unit) : v.toFixed(id === 'mix' ? 0 : 1) + unit; setParam(id, v.toFixed(2)); });
  }
  CTL.forEach(([id, , unit]) => { if (recent(id)) return; const r = $('r_' + id); r.value = toPos(id, P[id]); r.style.setProperty('--p', r.value / 10 + '%'); $('v_' + id).textContent = P[id].text + (P[id].text === 'Off' ? '' : unit); });
  $('sum').textContent = CTL.map(([id, name]) => name + ' ' + P[id].text).join(' · ');
  if (!recent('quality')) { const q = Math.round(P.quality.value); document.querySelectorAll('#quality button').forEach((b, i) => b.classList.toggle('on', i === q)); }
  if (!recent('normalize')) $('norm').setAttribute('aria-checked', P.normalize.value >= 0.5);
}
document.querySelectorAll('#quality button').forEach((b, i) => b.onclick = () => { document.querySelectorAll('#quality button').forEach(x => x.classList.toggle('on', x === b)); setParam('quality', i); });
$('norm').onclick = () => { const on = $('norm').getAttribute('aria-checked') !== 'true'; $('norm').setAttribute('aria-checked', on); setParam('normalize', on ? 1 : 0); };

// ---- what is playing, levels
function current() {   // what was just picked here shows at once, before the plugin confirms it
  const c = S.current;
  if (sel && Date.now() - sel.t < 2500 && !(c && c.key === sel.key && c.file === sel.file)) {
    const f = S.folders.find(f => f.key === sel.key), m = f && f.models.find(m => m.file === sel.file);
    if (m) return { key: sel.key, file: sel.file, name: m.name, folder: f.name, pending: true };
  }
  return c;
}
function flag(text, bad) { $('flag').hidden = !text; $('flag').textContent = text || ''; $('flag').className = 'flag' + (bad ? ' bad' : ''); }
function deck() {
  const c = current(), e = S.engine;
  $('dot').className = 'dot'; $('where').textContent = location.host;
  flat = []; S.folders.forEach(f => f.models.forEach(m => flat.push({ key: f.key, file: m.file })));
  $('prev').disabled = $('next').disabled = flat.length < 2;
  const f = c && S.folders.find(f => f.key === c.key), at = f ? f.models.findIndex(m => m.file === c.file) : -1;
  $('name').textContent = c ? c.name : 'No profile selected';
  $('sub').textContent = c ? c.folder + (at >= 0 ? `  ·  ${at + 1} of ${f.models.length}` : '') : (flat.length ? 'Pick one from the card' : 'Audio passes through unchanged');
  if (!c || c.pending) flag('');
  else if (c.error) flag('This profile could not be loaded.', true);
  else if (S.bypassed) flag('Too heavy for the CPU, so it is bypassed. Quality: Lite in the settings may help.', true);
  else flag(e.current ? '' : 'Loading...');
}
function meters() {
  const pct = (db, lo) => Math.max(0, Math.min(100, 100 * (db - lo) / (0 - lo))), dB = v => v < -90 ? 'quiet' : v.toFixed(0) + ' dB';
  const set = (id, w, txt, hot, tip) => { $(id + 'Bar').firstChild.style.width = w + '%'; $(id + 'Bar').classList.toggle('hot', hot); $(id + 'Txt').textContent = txt; $(id + 'M').title = tip; };
  const e = S.engine, cpu = `${Math.round(S.cpu * 100)} % of one core, peaks ${Math.round(S.cpuPeak * 100)} %` + (S.overruns ? `, ${S.overruns} late blocks` : '');
  const tin = `${dB(S.inRms)} average, ${dB(S.inPeak)} peak`, tout = `${dB(S.outPeak)} peak`;
  set('in', pct(S.inRms, -60), dB(S.inRms), S.inPeak > -1, 'Into the model: ' + tin);
  set('out', pct(S.outPeak, -60), dB(S.outPeak), S.outPeak > -0.5, 'Plugin output: ' + tout);
  set('cpu', Math.min(100, S.cpuPeak * 100), Math.round(S.cpu * 100) + ' %', S.cpuPeak > 0.8, 'CPU: ' + cpu);
  $('fEng').textContent = e.kind ? `${e.kind}${e.lite ? ' (light version)' : ''}, at ${e.rate} Hz` : 'None: audio passes through';
  $('fIn').textContent = tin; $('fOut').textContent = tout; $('fCpu').textContent = cpu; $('fHost').textContent = `${S.rate} Hz, ${S.block}-frame blocks`;
}
async function play(key, file) {
  sel = { key, file, t: Date.now() }; if (S) { deck(); library(); }
  try { await call('POST', '/api/select?key=' + encodeURIComponent(key) + '&file=' + encodeURIComponent(file)); setTimeout(refresh, 400); } catch (e) { sel = null; toast(e.message, true); refresh(); }
}
function step(d) {
  if (!S || !flat.length) return;
  const c = current(), i = c ? flat.findIndex(x => x.key === c.key && x.file === c.file) : -1, n = flat[i < 0 ? 0 : (i + d + flat.length) % flat.length];
  play(n.key, n.file);
}
$('prev').onclick = () => step(-1); $('next').onclick = () => step(1);

// ---- the card's library
function library() {
  const c = current(), total = flat.length;
  if (open === null) { open = new Set(); if (total <= 12) S.folders.forEach(f => open.add(f.key)); else if (c) open.add(c.key); else if (S.folders[0]) open.add(S.folders[0].key); }
  if (c && c.key !== lastKey) { open.add(c.key); lastKey = c.key; }
  const words = $('find').value.toLowerCase().split(/\s+/).filter(Boolean), hit = (f, m) => words.every(w => (f.name + ' ' + m.name + ' ' + m.kind).toLowerCase().includes(w));
  const sig = JSON.stringify([S.folders, c && [c.key, c.file], words, [...open]]);
  if (sig === libSig) return; libSig = sig;
  const groups = S.folders.filter(f => f.models.some(m => hit(f, m)));
  $('edit').hidden = !S.folders.some(f => f.dir != null);
  $('lib').innerHTML = !total ? '<p class="empty">No profiles on the card yet.<br>Get some from <button class="linkish" data-go="t3k">TONE3000</button>, the <button class="linkish" data-go="com">community collection</button>, or <button class="linkish" data-go="up">your own files</button>.</p>'
    : !groups.length ? '<p class="empty">Nothing on the card matches.</p>'
    : groups.map(f => { const shut = !words.length && !open.has(f.key);
        return `<div class="group${shut ? ' shut' : ''}"><button class="ghead" data-g="${esc(f.key)}" aria-expanded="${!shut}">${CHEV}<span class="t">${esc(f.name)}</span><span class="n">${f.models.length}</span></button><div class="rows">` +
          f.models.map((m, i) => !hit(f, m) ? '' : `<div class="row${c && c.key === f.key && c.file === m.file ? ' now' : ''}"><button class="play" data-k="${esc(f.key)}" data-f="${esc(m.file)}"><span class="i">${i + 1}</span><span class="t">${esc(m.name)}</span>${m.kind ? `<span class="k">${esc(m.kind)}</span>` : ''}</button>` +
            (f.dir == null ? '' : `<button class="icon del" data-d="${esc(f.dir)}" data-f="${esc(m.file)}" title="Remove from the card" aria-label="Remove ${esc(m.name)}">${BIN}</button>`) + '</div>').join('') + '</div></div>'; }).join('');
}
$('lib').onclick = async e => {
  const go = e.target.closest('[data-go]'), g = e.target.closest('.ghead'), p = e.target.closest('.play'), d = e.target.closest('.del');
  if (go) return show(go.dataset.go);
  if (g) { open.has(g.dataset.g) ? open.delete(g.dataset.g) : open.add(g.dataset.g); return library(); }
  if (p) return play(p.dataset.k, p.dataset.f);
  if (!d) return;
  if (!d.classList.contains('sure')) { d.classList.add('sure'); d.textContent = 'Remove?'; return setTimeout(() => { libSig = ''; if (S) library(); }, 3000); }
  try { await call('DELETE', '/api/profile?folder=' + encodeURIComponent(d.dataset.d) + '&file=' + encodeURIComponent(d.dataset.f)); toast('Removed from the card.'); refresh(); } catch (err) { toast(err.message, true); }
};
$('edit').onclick = () => { const on = $('lib').classList.toggle('editing'); $('edit').textContent = on ? 'Done' : 'Edit'; };
$('find').oninput = () => { if (S) library(); };

async function refresh() {
  try { S = await (await fetch('/api/status')).json(); }
  catch (e) { $('dot').className = 'dot off'; flag('The plugin is not answering. Is NAM Player still loaded on the MPC?', true); return; }
  deck(); controls(); meters(); library();
  $('nocard').hidden = !!S.uploads;
}

// ---- sending profiles to the card
function cut(n) { if (n.length > 16) { const c = n.slice(0, 16), i = c.lastIndexOf(' '); n = (i >= 8 ? c.slice(0, i) : c).trim(); } return n || 'model'; }
// Short names that stay different: captures of one amp share a long prefix, which is reduced to its initials.
function shortNames(list) {
  const w = list.map(p => p.replace(/\.nam$/i, '').replace(/^[A-Z][a-z]+ [A-Z]\.? (?=\S)/, '').replace(/[^A-Za-z0-9 _+\-().&']/g, ' ').replace(/\s+/g, ' ').trim().split(' '));
  const groups = {}, out = [], seen = {};
  w.forEach((x, i) => { const k = x.slice(0, 2).join(' ').toLowerCase(); (groups[k] = groups[k] || []).push(i); });
  for (const m of Object.values(groups)) {
    let c = 0; const f = w[m[0]];
    if (m.length > 1) while (c < f.length - 1 && m.every(i => w[i].length > c + 1 && w[i][c].toLowerCase() === f[c].toLowerCase())) c++;
    for (const i of m) out[i] = cut(((c >= 2 ? w[i].slice(0, c).map(x => x[0].toUpperCase()).join('') : w[i].slice(0, c).join(' ')) + ' ' + w[i].slice(c).join(' ')).trim());
  }
  return out.map(n => { const k = n.toLowerCase(); seen[k] = (seen[k] || 0) + 1; return seen[k] > 1 ? n.slice(0, 15 - String(seen[k]).length).trim() + ' ' + seen[k] : n; });
}
async function send(folder, blob, name, about, play) {
  let q = '/api/profile?folder=' + encodeURIComponent(folder || 'My Profiles') + '&name=' + encodeURIComponent(name) + (play ? '&select=1' : '');
  if (about) q += '&about=' + encodeURIComponent(about);
  return call('PUT', q, blob);
}
async function upload(files) {
  const list = [...files].filter(f => /\.nam$/i.test(f.name)); let n = 0;
  if (!list.length) return toast('Only .nam files can be sent.', true);
  for (const f of list) { $('dropMsg').textContent = 'Sending ' + f.name + '...'; try { await send($('folder').value, f, f.name, '', ++n === list.length); } catch (e) { n--; toast(f.name + ': ' + e.message, true); } }
  $('dropMsg').textContent = 'Drop .nam files anywhere on this page';
  if (n) { toast(n === 1 ? 'Saved. Playing it now.' : n + ' profiles saved. Playing the last one.'); setTimeout(refresh, 400); }
}
const drop = $('drop'), hasFiles = e => e.dataTransfer && [...e.dataTransfer.types].includes('Files');
document.addEventListener('dragover', e => { if (!hasFiles(e)) return; e.preventDefault(); show('up'); drop.classList.add('over'); });
document.addEventListener('dragleave', e => { if (!e.relatedTarget) drop.classList.remove('over'); });
document.addEventListener('drop', e => { if (!hasFiles(e)) return; e.preventDefault(); drop.classList.remove('over'); upload(e.dataTransfer.files); });
$('pick').onchange = e => { upload(e.target.files); e.target.value = ''; };

// ---- TONE3000: sign in on tone3000.com in a new tab (OAuth, PKCE, "select tone" flow). The MPC catches the
// answer at /t3k/callback and this page collects it. Tokens stay in this browser tab.
const T3K = 'https://www.tone3000.com', b64 = a => btoa(String.fromCharCode.apply(null, a)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=/g, '');
function sha256(str) {   // this page is plain http, where browsers do not offer crypto.subtle
  const K = [], H = [], isP = n => { for (let i = 2; i * i <= n; i++) if (n % i === 0) return false; return true; }, frac = x => ((x - Math.floor(x)) * 4294967296) | 0;
  for (let n = 2; K.length < 64; n++) if (isP(n)) { if (H.length < 8) H.push(frac(Math.sqrt(n))); K.push(frac(Math.cbrt(n))); }
  const m = Array.from(new TextEncoder().encode(str)), bits = m.length * 8; m.push(0x80); while (m.length % 64 !== 56) m.push(0);
  for (let i = 7; i >= 0; i--) m.push(i > 3 ? 0 : (bits >>> (8 * i)) & 255);
  const R = (x, n) => (x >>> n) | (x << (32 - n));
  for (let o = 0; o < m.length; o += 64) {
    const w = []; for (let i = 0; i < 64; i++) w[i] = i < 16 ? (m[o + 4 * i] << 24) | (m[o + 4 * i + 1] << 16) | (m[o + 4 * i + 2] << 8) | m[o + 4 * i + 3]
      : (w[i - 16] + (R(w[i - 15], 7) ^ R(w[i - 15], 18) ^ (w[i - 15] >>> 3)) + w[i - 7] + (R(w[i - 2], 17) ^ R(w[i - 2], 19) ^ (w[i - 2] >>> 10))) | 0;
    let [a, b, c, d, e, f, g, h] = H;
    for (let i = 0; i < 64; i++) {
      const t1 = (h + (R(e, 6) ^ R(e, 11) ^ R(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i]) | 0, t2 = ((R(a, 2) ^ R(a, 13) ^ R(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    [a, b, c, d, e, f, g, h].forEach((v, i) => H[i] = (H[i] + v) | 0);
  }
  const out = []; H.forEach(v => out.push((v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255)); return out;
}
let t3 = { key: '', token: '', poll: null, redirect: '', arch: '2' };
try { t3.token = sessionStorage.getItem('t3k_token') || ''; } catch (e) {}
const keep = v => { try { v ? sessionStorage.setItem('t3k_token', v) : sessionStorage.removeItem('t3k_token'); } catch (e) {} };
const rnd = n => b64(crypto.getRandomValues(new Uint8Array(n)));
async function t3get(path) {
  const r = await fetch(T3K + path.replace(/^https?:\/\/[^/]+/, ''), { headers: { Authorization: 'Bearer ' + t3.token } });
  if (r.status === 401) { t3.token = ''; keep(''); throw new Error('TONE3000 session ended: browse again to sign in'); }
  if (!r.ok) throw new Error(r.status === 429 ? 'TONE3000 is rate-limiting: wait a minute' : 'TONE3000 answered ' + r.status);
  return r;
}
async function t3init() {
  try { t3.key = (await (await fetch('/api/t3k/key')).json()).key || ''; } catch (e) {}
  try { const l = await (await fetch('/api/link')).json(); t3.redirect = 'http://' + (l.address || location.hostname) + ':' + l.port + '/t3k/callback'; } catch (e) { t3.redirect = location.origin + '/t3k/callback'; }
  t3render();
}
function t3render(msg) {
  $('t3k').innerHTML = !t3.key
    ? '<div class="card pad"><p>To browse TONE3000 from here, paste your <b>publishable</b> API key. It is on tone3000.com under Settings, API Keys, and starts with <code>t3k_pub_</code>. It is saved on the MPC\'s card.</p><div class="line"><input type="text" class="field grow" id="t3key" placeholder="t3k_pub_..." autocomplete="off" spellcheck="false"><button class="btn primary" id="t3save">Save</button></div></div>'
    : `<div class="line wrap"><button class="btn primary" id="t3go">Browse TONE3000</button><select class="field" id="t3arch" aria-label="Kind of profile"><option value="2">A2 profiles</option><option value="1">A1 profiles</option><option value="custom">Custom</option></select></div>
       <p class="small muted">${msg || 'TONE3000 opens in a new tab. Pick a tone there and it shows up here.'}</p>
       <details class="small muted"><summary>Sign-in trouble</summary><div><p>If TONE3000 refuses the return address, add <code>${esc(t3.redirect)}</code> as a redirect URI on your key.</p><button class="btn" id="t3forget">Use a different key</button></div></details>`;
  if ($('t3save')) $('t3save').onclick = async () => { try { await call('POST', '/api/t3k/key?key=' + encodeURIComponent($('t3key').value.trim())); t3.key = $('t3key').value.trim(); t3render(); } catch (e) { toast(e.message, true); } };
  if ($('t3forget')) $('t3forget').onclick = async () => { try { await call('POST', '/api/t3k/key?key='); t3.key = ''; $('tone').innerHTML = ''; t3render(); } catch (e) { toast(e.message, true); } };
  if ($('t3arch')) { $('t3arch').value = t3.arch; $('t3arch').onchange = e => t3.arch = e.target.value; }
  if ($('t3go')) $('t3go').onclick = t3browse;
}
function t3browse() {
  const verifier = rnd(32), state = rnd(16), arch = t3.arch;
  const q = new URLSearchParams({ client_id: t3.key, redirect_uri: t3.redirect, response_type: 'code', code_challenge: b64(sha256(verifier)), code_challenge_method: 'S256', state, prompt: 'select_tone', format: 'nam', architecture: arch, preview: 'true', menubar: 'true' });
  const tab = window.open(T3K + '/api/v1/oauth/authorize?' + q, '_blank');
  if (!tab) return toast('The browser blocked the new tab: allow pop-ups for this page', true);
  t3render('Waiting for you to pick a tone on TONE3000...');
  clearInterval(t3.poll); const started = Date.now();
  t3.poll = setInterval(async () => {
    if (Date.now() - started > 15 * 60000) { clearInterval(t3.poll); return t3render(); }
    let r; try { r = await (await fetch('/api/t3k/poll?state=' + encodeURIComponent(state))).json(); } catch (e) { return; }
    if (r.pending) return;
    clearInterval(t3.poll);
    try {
      if (r.error) throw new Error('TONE3000: ' + r.error);
      if (!r.code) throw new Error('Sign-in was cancelled');
      const tr = await fetch(T3K + '/api/v1/oauth/token', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams({ grant_type: 'authorization_code', code: r.code, code_verifier: verifier, redirect_uri: t3.redirect, client_id: t3.key }) });
      if (!tr.ok) throw new Error('TONE3000 did not accept the sign-in (' + tr.status + ')');
      t3.token = (await tr.json()).access_token; keep(t3.token);
      t3render(); if (r.tone_id) await t3tone(r.tone_id, arch); else toast('Signed in. No tone was picked.');
    } catch (e) { t3render(); toast(e.message, true); }
  }, 1500);
}
async function t3tone(id, arch) {
  const tone = await (await t3get('/api/v1/tones/' + encodeURIComponent(id) + '?architecture=' + arch)).json();
  const models = (await (await t3get('/api/v1/models?tone_id=' + encodeURIComponent(id) + '&architecture=' + arch + '&page_size=300')).json()).data || [];
  const who = (tone.user && (tone.user.display_name || tone.user.username)) || '', sn = shortNames(models.map(m => m.name || 'model'));
  $('tone').innerHTML = `<div><b>${esc(tone.title)}</b><div class="muted small">by ${esc(who)}  ·  ${esc(tone.gear)}  ·  licence ${esc(tone.license)}</div></div>` +
    `<div class="line"><label class="small muted" for="t3folder">Save to folder</label><input type="text" class="field grow" id="t3folder" value="${esc(cut((tone.title || 'TONE3000').replace(/[^A-Za-z0-9 _+\-().&']/g, ' ').replace(/\s+/g, ' ').trim()))}" maxlength="24"></div>` +
    (models.length ? '<div class="card">' + models.map((m, i) => `<div class="srow"><span class="t">${esc(m.name)} <span class="muted">${esc(m.size || '')}</span></span><input type="text" class="field" value="${esc(sn[i])}" maxlength="24" aria-label="Name on the MPC"><button class="btn soft t3send" data-i="${i}">Send</button></div>`).join('') + '</div>'
                   : '<p class="flag bad">No models of this kind in this tone.</p>');
  document.querySelectorAll('.t3send').forEach(b => b.onclick = async () => {
    const m = models[b.dataset.i]; b.disabled = true; b.textContent = '...';
    try {
      const blob = await (await t3get(m.model_url)).blob();
      await send($('t3folder').value || 'TONE3000', blob, b.previousElementSibling.value, `from TONE3000 ${tone.url || ''}; tone: ${tone.title}; creator: ${who}; licence: ${tone.license}`, true);
      toast('Saved. Playing it now.'); setTimeout(refresh, 400);
    } catch (e) { toast(e.message, true); }
    b.disabled = false; b.textContent = 'Send';
  });
}

// ---- the community collection
function renderCat() {
  const w = $('q').value.toLowerCase().split(/\s+/).filter(Boolean), all = catalog.filter(p => w.every(x => p.toLowerCase().includes(x))), rows = all.slice(0, 60);
  $('cat').innerHTML = rows.map(p => `<div class="srow"><span class="t">${esc(p.replace(/\.nam$/i, ''))}</span><input type="text" class="field" value="${esc(names[p] || 'model')}" maxlength="24" aria-label="Name on the MPC"><button class="btn soft get" data-p="${esc(p)}">Send</button></div>`).join('') || '<p class="empty">Nothing matches.</p>';
  $('catNote').textContent = (all.length > rows.length ? `Showing ${rows.length} of ${all.length}: type to narrow it down. ` : '') + `${catalog.length} captures from github.com/pelennor2170/NAM_models (GPL-3.0), fetched by this browser. All are the older A1 kind, heavier than A2.`;
}
$('cat').onclick = async e => {
  const b = e.target.closest('.get'); if (!b || busy) return;
  busy = true; b.disabled = true; b.textContent = '...';
  try {
    const r = await fetch(RAW + encodeURIComponent(b.dataset.p));
    if (!r.ok) throw new Error('GitHub answered ' + r.status);
    await send($('comFolder').value || 'Community', await r.blob(), b.previousElementSibling.value, 'from https://github.com/pelennor2170/NAM_models (GPL-3.0): ' + b.dataset.p, true);
    toast('Saved. Playing it now.'); setTimeout(refresh, 400);
  } catch (err) { toast(/fetch|network|load/i.test(err.message) ? 'This device cannot reach GitHub right now.' : err.message, true); }
  busy = false; b.disabled = false; b.textContent = 'Send';
};
$('q').oninput = renderCat;

(async () => {
  await refresh(); setInterval(() => { if (!document.hidden) refresh(); }, 1000); t3init();
  try { const t = await (await fetch('/catalog.txt')).text(); catalog = t.split('\n').map(x => x.trim()).filter(Boolean); shortNames(catalog).forEach((n, i) => names[catalog[i]] = n); } catch (e) {}
  renderCat();
})();
</script></body></html>
)PAGE";

} // namespace namplayer

#endif // NAMPLAYER_LINK_PAGE_H
