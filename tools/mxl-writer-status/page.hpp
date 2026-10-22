// The ring view mxl-writer-status serves at /: one self-contained page that
// draws each flow's ring and commit timing from the /stream events and its
// counters from /status. Same look as mxl-reader's ring view.

#pragma once

namespace
{
    constexpr char const* RingPage = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>mxl-writer ring</title>
<style>
  :root {
    --bg: #002f2d; --panel: #004643; --ink: #ffffff; --dim: rgba(255,255,255,0.6);
    --ok: #c8f169; --warn: #ffd966; --bad: #f05012; --miss: #b48cff; --ahead: #6fd3ff;
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--ink);
         font: 14px/1.4 system-ui, -apple-system, 'Segoe UI', sans-serif; }
  header { display: flex; align-items: baseline; gap: 14px; flex-wrap: wrap; padding: 14px 20px; }
  header h1 { font-size: 16px; letter-spacing: 0.06em; text-transform: uppercase; margin: 0; }
  header .version { color: var(--dim); font-size: 12px; }
  header .conn { margin-left: auto; font-size: 12px; }
  .tabs { display: flex; gap: 8px; flex-wrap: wrap; padding: 0 20px 12px; }
  .tab { background: rgba(255,255,255,0.08); color: var(--ink); border: 0; border-radius: 999px;
         padding: 6px 14px; cursor: pointer; font: inherit; font-size: 13px; }
  .tab.on { background: var(--ok); color: #0b090d; font-weight: 700; }
  main { display: grid; grid-template-columns: minmax(300px, 460px) minmax(0, 1fr); gap: 16px; padding: 0 20px 20px; }
  @media (max-width: 900px) { main { grid-template-columns: minmax(0, 1fr); } }
  .panel { background: var(--panel); border-radius: 18px; padding: 16px; }
  .ring canvas { width: 100%; aspect-ratio: 1; display: block; }
  .side { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
  .numbers { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 10px; }
  .num { background: rgba(255,255,255,0.06); border-radius: 12px; padding: 8px 12px; }
  .num .k { font-size: 10px; text-transform: uppercase; letter-spacing: 0.05em; color: var(--dim); }
  .num .v { font-size: 22px; font-weight: 800; font-variant-numeric: tabular-nums; }
  .num .v small { font-size: 12px; font-weight: 600; color: var(--dim); }
  .ringinfo { margin-top: 12px; grid-template-columns: repeat(4, minmax(0, 1fr)); }
  .ringinfo .num .v { font-size: 17px; }
  .strip canvas { width: 100%; height: 200px; display: block; }
  .legend { display: flex; flex-wrap: wrap; gap: 12px; font-size: 12px; color: var(--dim); margin-top: 8px; }
  .legend i { display: inline-block; width: 10px; height: 10px; border-radius: 2px; margin-right: 5px; vertical-align: -1px; }
  .chips { display: flex; flex-wrap: wrap; gap: 6px; }
  .chip { font-size: 12px; padding: 2px 10px; border-radius: 999px; background: rgba(255,255,255,0.08); }
  .chip.ok { color: var(--ok); } .chip.warn { color: var(--warn); } .chip.bad { color: #ff9a73; }
  .chip.zero { opacity: 0.4; }
  .info { color: var(--dim); font-size: 12px; margin-top: 10px; overflow-wrap: anywhere; }
  .err { color: #ff9a73; font-size: 13px; margin-top: 8px; }
  h2 { font-size: 12px; text-transform: uppercase; letter-spacing: 0.06em; margin: 0 0 10px; color: var(--dim); }
</style>
</head>
<body>
<header>
  <h1>Writer ring</h1>
  <span class="version" id="version"></span>
  <span class="conn" id="conn">connecting</span>
</header>
<div class="tabs" id="tabs"></div>
<main>
  <section class="panel ring">
    <h2 id="ringTitle">Ring</h2>
    <canvas id="ring" width="900" height="900"></canvas>
    <div class="numbers ringinfo" id="ringInfo"></div>
    <div class="legend">
      <span><i style="background:var(--ahead)"></i>ahead of media time</span>
      <span><i style="background:var(--ok)"></i>within a grain or commit batch</span>
      <span><i style="background:var(--warn)"></i>within a reader's grace</span>
      <span><i style="background:var(--bad)"></i>past it</span>
      <span><i style="background:var(--miss)"></i>never written</span>
      <span><i style="background:#ff9a73"></i>head stepped back</span>
      <span>&#9650; head &nbsp; &#9661; media clock</span>
    </div>
  </section>
  <section class="side">
    <div class="panel">
      <h2>Last second</h2>
      <div class="numbers" id="numbers"></div>
    </div>
    <div class="panel strip">
      <h2>Commit against its media timestamp, last 6 s, in ms</h2>
      <canvas id="after" width="1600" height="400"></canvas>
      <div class="legend">
        <span><i style="background:var(--ok)"></i>commit</span>
        <span><i style="background:#ff9a73"></i>head stepped back</span>
        <span><i style="background:var(--bad)"></i>stall</span>
      </div>
    </div>
    <div class="panel strip">
      <h2>Interval between commits, last 6 s, in ms</h2>
      <canvas id="interval" width="1600" height="400"></canvas>
    </div>
    <div class="panel">
      <h2>Counters since the status tool started</h2>
      <div class="chips" id="chips"></div>
      <div class="info" id="info"></div>
      <div class="err" id="err"></div>
    </div>
  </section>
</main>
<script>
"use strict";
const WINDOW_MS = 6000;
const COMMITS = 'kgrb';
const flows = new Map();      // id -> { meta, events: [], slots: Map(segment -> event) }
let selected = null;
let skew = 0;                 // server clock minus client clock, in ms
let lastBatchAt = 0;

const colorFor = (ev, meta) => {
  if (ev.kind === 'm') return '#b48cff';
  if (ev.kind === 'b') return '#ff9a73';
  if (ev.after < 0) return '#6fd3ff';
  if (ev.after <= meta.spanMs + 2) return '#c8f169';
  if (ev.after <= meta.graceMs) return '#ffd966';
  return '#f05012';
};

function segments(meta) { return Math.max(1, Math.min(meta.ring || 1, 360)); }
function segmentOf(index, meta) {
  const ring = meta.ring || 1;
  // Multiply before dividing: (k / ring) * slots rounds just below a whole
  // number for some k, which leaves a slot that never receives a grain.
  return Math.floor((index % ring) * segments(meta) / ring);
}

// What one commit wrote into the ring: the grain at the head, and for a head
// that jumped, the grains it passed without writing; for samples, every slot
// from the previous head up to this one.
function paint(flow, ev) {
  const meta = flow.meta, ring = meta.ring || 1;
  if (meta.unit === 'samples') {
    const span = Math.min(Math.max(ev.step, 1), ring);
    let s = segmentOf(ev.head - span, meta);
    const end = segmentOf(ev.head - 1, meta);
    for (let n = 0; n <= segments(meta); n++) {
      flow.slots.set(s, ev);
      if (s === end) break;
      s = (s + 1) % segments(meta);
    }
    return;
  }
  if (ev.kind === 'g') {
    for (let i = Math.max(ev.head - ev.step + 1, ev.head - ring + 1); i < ev.head; i++) {
      flow.slots.set(segmentOf(i, meta), { kind: 'm', at: ev.at });
    }
  }
  flow.slots.set(segmentOf(ev.head, meta), ev);
}

function onBatch(batch) {
  skew = batch.now - Date.now();
  lastBatchAt = performance.now();
  for (const f of batch.flows) {
    let flow = flows.get(f.id);
    if (!flow) {
      flow = { meta: f, events: [], slots: new Map() };
      flows.set(f.id, flow);
      if (!selected) selected = f.id;
      renderTabs();
    }
    if (flow.meta.ring !== f.ring) flow.slots.clear();
    flow.meta = f;
    for (const [head, at, after, step, kind] of f.ev) {
      const ev = { head, at, after, step, kind };
      flow.events.push(ev);
      if (COMMITS.includes(kind)) paint(flow, ev);
    }
    const cutoff = batch.now - WINDOW_MS - 1000;
    while (flow.events.length && flow.events[0].at < cutoff) flow.events.shift();
  }
}

function renderTabs() {
  const tabs = document.getElementById('tabs');
  tabs.innerHTML = '';
  for (const [id, flow] of flows) {
    const b = document.createElement('button');
    b.className = 'tab' + (id === selected ? ' on' : '');
    b.textContent = `${flow.meta.label || flow.meta.format || id.slice(0, 8)}  ${id.slice(0, 8)}`;
    b.onclick = () => { selected = id; renderTabs(); };
    tabs.appendChild(b);
  }
}

function fmtBytes(n) {
  if (!n) return ['--', ''];
  if (n >= 1e9) return [(n / 1e9).toFixed(2), 'GB'];
  if (n >= 1e6) return [(n / 1e6).toFixed(n >= 1e8 ? 0 : 1), 'MB'];
  if (n >= 1e3) return [(n / 1e3).toFixed(1), 'KB'];
  return [String(n), 'B'];
}

function cellsHtml(cells) {
  return cells.map(([k, v, u]) => `<div class="num"><div class="k">${k}</div><div class="v">${v} <small>${u}</small></div></div>`).join('');
}

// The ring itself: how many slots, the payload of one, all of them, and how
// much media time that holds.
function drawRingInfo(meta) {
  const ring = meta.ring || 0;
  const slot = fmtBytes(meta.slotBytes);
  const total = fmtBytes(ring * (meta.slotBytes || 0));
  const span = ring && meta.periodMs ? ring * meta.periodMs / 1000 : null;
  document.getElementById('ringInfo').innerHTML = cellsHtml([
    ['slots', ring || '--', meta.unit || ''],
    ['slot size', slot[0], slot[1]],
    ['ring total', total[0], total[1]],
    ['history', span === null ? '--' : span.toFixed(2), 's'],
  ]);
}

function percentile(sorted, p) {
  if (!sorted.length) return null;
  return sorted[Math.min(sorted.length - 1, Math.floor(p * sorted.length))];
}

function fmtMs(v) { return v === null || v === undefined ? '--' : v.toFixed(1); }

function drawRing(flow, now) {
  const c = document.getElementById('ring');
  const g = c.getContext('2d');
  const W = c.width, cx = W / 2, cy = W / 2, R = W * 0.42, thick = W * 0.09;
  g.clearRect(0, 0, W, W);
  const meta = flow.meta;
  const n = segments(meta);
  const ringMs = (meta.ring || 1) * meta.periodMs;
  const step = (Math.PI * 2) / n;
  const gap = n > 120 ? 0 : step * 0.08;
  for (let s = 0; s < n; s++) {
    const ev = flow.slots.get(s);
    const a0 = -Math.PI / 2 + s * step + gap / 2, a1 = a0 + step - gap;
    g.beginPath();
    g.arc(cx, cy, R, a0, a1);
    g.arc(cx, cy, R - thick, a1, a0, true);
    g.closePath();
    if (ev) {
      const age = now - ev.at;
      g.globalAlpha = Math.max(0.18, 1 - age / Math.max(ringMs, 200) * 0.8);
      g.fillStyle = colorFor(ev, meta);
    } else {
      g.globalAlpha = 1;
      g.fillStyle = 'rgba(255,255,255,0.06)';
    }
    g.fill();
  }
  g.globalAlpha = 1;
  // Head and clock move between batches at one grain per period.
  const drift = meta.periodMs > 0 && meta.unit === 'grains' && meta.state === 'open'
    ? Math.min(Math.floor((performance.now() - lastBatchAt) / meta.periodMs), 3) : 0;
  const angleOf = (index) => -Math.PI / 2 + ((index % (meta.ring || 1)) / (meta.ring || 1)) * Math.PI * 2 + step / 2;
  const marker = (index, outside, color) => {
    const a = angleOf(index);
    const r = outside ? R + W * 0.025 : R - thick - W * 0.025;
    const tip = outside ? R + 2 : R - thick - 2;
    g.beginPath();
    g.moveTo(cx + Math.cos(a) * tip, cy + Math.sin(a) * tip);
    g.lineTo(cx + Math.cos(a - 0.03) * r, cy + Math.sin(a - 0.03) * r);
    g.lineTo(cx + Math.cos(a + 0.03) * r, cy + Math.sin(a + 0.03) * r);
    g.closePath();
    g.fillStyle = color;
    g.fill();
  };
  if (meta.head) marker(meta.head + drift, true, '#ffffff');
  if (meta.clock) marker(meta.clock + drift, false, '#c8f169');
  // Centre: where the head stands against the media clock, large.
  const lead = meta.head && meta.clock ? (meta.head - meta.clock) * meta.periodMs : null;
  g.fillStyle = '#ffffff';
  g.textAlign = 'center';
  g.font = `800 ${W * 0.11}px system-ui, sans-serif`;
  g.fillText(lead === null ? '--' : (lead > 0 ? '+' : '') + lead.toFixed(1), cx, cy + W * 0.02);
  g.font = `600 ${W * 0.035}px system-ui, sans-serif`;
  g.fillStyle = 'rgba(255,255,255,0.6)';
  g.fillText('ms head vs media clock', cx, cy + W * 0.075);
  g.fillText(`${meta.ring || '?'} ${meta.unit || 'slots'}, ${meta.state}`, cx, cy - W * 0.1);
}

// A linear strip over the last six seconds, scaled to what it shows, with
// dashed reference lines.
function drawStrip(id, now, points, refs, marks) {
  const c = document.getElementById(id);
  const g = c.getContext('2d');
  const W = c.width, H = c.height, pad = 56;
  g.clearRect(0, 0, W, H);
  const from = now - WINDOW_MS;
  const ys = points.map((p) => p.y).concat(refs.map((r) => r[0]), [0]);
  let lo = Math.min(...ys), hi = Math.max(...ys);
  if (hi - lo < 1) hi = lo + 1;
  const span = hi - lo;
  lo -= span * 0.08; hi += span * 0.08;
  const x = (t) => pad + ((t - from) / WINDOW_MS) * (W - pad - 10);
  const y = (v) => H - 36 - ((v - lo) / (hi - lo)) * (H - 36 - 12);
  g.font = '20px system-ui, sans-serif';
  g.textAlign = 'right';
  for (let i = 0; i <= 4; i++) {
    const v = lo + (hi - lo) * i / 4;
    g.strokeStyle = 'rgba(255,255,255,0.08)'; g.beginPath(); g.moveTo(pad, y(v)); g.lineTo(W - 10, y(v)); g.stroke();
    g.fillStyle = 'rgba(255,255,255,0.5)'; g.fillText(Math.abs(hi - lo) < 10 ? v.toFixed(1) : v.toFixed(0), pad - 6, y(v) + 6);
  }
  for (const [v, label, color] of refs) {
    g.strokeStyle = color; g.setLineDash([8, 8]); g.beginPath();
    g.moveTo(pad, y(v)); g.lineTo(W - 10, y(v)); g.stroke(); g.setLineDash([]);
    g.fillStyle = color; g.textAlign = 'left'; g.fillText(`${label} ${v.toFixed(1)} ms`, pad + 6, y(v) - 6);
  }
  g.textAlign = 'center';
  g.fillStyle = 'rgba(255,255,255,0.5)';
  for (let s = 0; s <= 6; s++) g.fillText(s === 6 ? 'now' : `-${6 - s}s`, x(from + s * 1000), H - 8);
  for (const m of marks) {
    g.fillStyle = m.color;
    g.fillRect(x(m.at) - 2, 12, 4, H - 48);
  }
  const r = Math.max(2, Math.min(6, (W - pad) / Math.max(points.length, 1) * 0.35));
  for (const p of points) {
    g.fillStyle = p.color;
    g.beginPath(); g.arc(x(p.at), y(p.y), r, 0, Math.PI * 2); g.fill();
  }
}

function commitsSince(flow, from) {
  return flow.events.filter((e) => e.at >= from && COMMITS.includes(e.kind));
}

function intervals(commits) {
  const out = [];
  for (let i = 1; i < commits.length; i++) out.push({ at: commits[i].at, y: commits[i].at - commits[i - 1].at });
  return out;
}

function drawStrips(flow, now) {
  const meta = flow.meta;
  const span = meta.unit === 'samples' ? 'commit batch' : 'period';
  const commits = commitsSince(flow, now - WINDOW_MS - 1000);
  const visible = commits.filter((e) => e.at >= now - WINDOW_MS);
  const marks = flow.events.filter((e) => e.at >= now - WINDOW_MS && (e.kind === 't' || e.kind === 'b'))
    .map((e) => ({ at: e.at, color: e.kind === 't' ? 'rgba(240,80,18,0.85)' : 'rgba(255,154,115,0.6)' }));
  drawStrip('after', now, visible.map((e) => ({ at: e.at, y: e.after, color: colorFor(e, meta) })),
    [[meta.spanMs, span, 'rgba(200,241,105,0.5)'], [meta.graceMs, 'grace', 'rgba(240,80,18,0.6)']], marks);
  drawStrip('interval', now, intervals(commits).filter((p) => p.at >= now - WINDOW_MS).map((p) => ({ ...p, color: '#6fd3ff' })),
    [[meta.spanMs, span, 'rgba(200,241,105,0.5)']], marks);
}

// How fast the head moved over the last second, from the commit times the
// tool recorded, not from when the page heard of them.
function headRate(commits) {
  let first = commits.length - 1;
  while (first > 0 && commits[first].kind !== 'b') first--;
  const base = commits[first], last = commits[commits.length - 1];
  if (!base || base === last || last.at <= base.at) return null;
  return (last.head - base.head) * 1000 / (last.at - base.at);
}

function drawNumbers(flow, now) {
  const meta = flow.meta;
  const commits = commitsSince(flow, now - 1000);
  const gaps = intervals(commits).map((p) => p.y).sort((a, b) => a - b);
  const after = commits.map((e) => e.after).sort((a, b) => a - b);
  const last = commits[commits.length - 1];
  const rate = headRate(commits);
  const lead = meta.head && meta.clock ? (meta.head - meta.clock) : null;
  const unit = meta.unit || '';
  document.getElementById('numbers').innerHTML = cellsHtml([
    ['head rate', rate === null ? '--' : rate.toFixed(rate >= 1000 ? 0 : 2), `${unit}/s of ${meta.rate ? meta.rate.toFixed(meta.rate >= 1000 ? 0 : 2) : '--'}`],
    ['commits', commits.length, '/s'],
    ['interval p50', fmtMs(percentile(gaps, 0.5)), 'ms'],
    ['interval max', fmtMs(gaps.length ? gaps[gaps.length - 1] : null), 'ms'],
    ['commit p50', fmtMs(percentile(after, 0.5)), 'ms after media'],
    ['commit max', fmtMs(after.length ? after[after.length - 1] : null), 'ms after media'],
    ['head vs clock', lead === null ? '--' : (lead * meta.periodMs).toFixed(1), lead === null ? '' : `ms, ${lead} ${unit}`],
    ['last write', last ? (now - last.at).toFixed(0) : '--', 'ms ago'],
  ]);
}

let numbersAt = 0;
function frame() {
  const now = Date.now() + skew;
  const flow = flows.get(selected);
  if (flow) {
    const name = flow.meta.label || flow.meta.id;
    document.getElementById('ringTitle').textContent = flow.meta.format ? `Ring of ${name}, ${flow.meta.format}` : `Ring of ${name}`;
    drawRing(flow, now);
    drawStrips(flow, now);
    // Four times a second: faster than that, digits blur instead of reading.
    if (Date.now() - numbersAt > 250) {
      numbersAt = Date.now();
      drawNumbers(flow, now);
      drawRingInfo(flow.meta);
    }
  }
  requestAnimationFrame(frame);
}

async function pollStatus() {
  try {
    const status = await (await fetch('status', { cache: 'no-store' })).json();
    document.getElementById('version').textContent = status.version || '';
    const f = status.flows.find((x) => x.id === selected) || status.flows[0];
    if (f) {
      const chips = [['commits', f.commits, 'ok'], ['skipped', f.skipped, 'bad'], ['backward steps', f.backwardSteps, 'bad'],
        ['stalls', f.stalls, 'warn'], ['opens', f.opens, 'ok'], ['lost', f.lost, 'warn']];
      document.getElementById('chips').innerHTML = chips
        .map(([k, v, sev]) => `<span class="chip ${v ? sev : 'zero'}">${k} <b>${v}</b></span>`).join('');
      const info = [];
      if (f.mediaType) info.push(`${f.mediaType} ${f.format}`);
      if (f.groupHint) info.push(`group ${f.groupHint}`);
      if (f.unit) info.push(`commit batch ${f.commitBatchHint}, sync batch ${f.syncBatchHint}, ${f.payloadLocation} memory`);
      // libmxl keeps no read time for samples, so /status gives none.
      if (f.readAgeSeconds !== undefined) info.push(`last read on this node ${f.readAgeSeconds.toFixed(1)} s ago`);
      info.push(f.writing ? 'writing' : 'not writing');
      document.getElementById('info').textContent = `${f.id}: ${info.join(', ')}`;
      document.getElementById('err').textContent = f.lastError
        ? `last error ${f.lastError.secondsAgo.toFixed(0)} s ago: ${f.lastError.message}` : '';
    }
  } catch (e) { /* the stream says whether the tool is there */ }
}
setInterval(pollStatus, 1000);
pollStatus();

function connect() {
  const conn = document.getElementById('conn');
  const es = new EventSource('stream');
  es.onopen = () => { conn.textContent = 'live'; conn.style.color = '#c8f169'; };
  es.onmessage = (m) => onBatch(JSON.parse(m.data));
  es.onerror = () => { conn.textContent = 'reconnecting'; conn.style.color = '#ffd966'; };
}
connect();
requestAnimationFrame(frame);
</script>
</body>
</html>
)PAGE";
}
