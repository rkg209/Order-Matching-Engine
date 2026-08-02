// Spec 010 T6 -- plain JS, no build step, no framework. Two jobs: keep a reconnecting WebSocket
// alive and draw whatever the latest message said onto two canvases at a fixed frame rate,
// decoupled from message arrival rate (the server already coalesces to ~20 Hz for the book and
// >=1 Hz for latency -- this file just redraws from the latest snapshot it has).

const PRICE_SCALE = 10000; // velox prices are scaled int64 (price * 10000) -- see common/types.hpp

const state = {
  book: { seq: 0, bids: [], asks: [], trades: [], tradeCount: 0 },
  lat: null,
  lastBookAt: 0,
  lastLatAt: 0,
  prevLevels: new Map(), // "side:price" -> qty, for the flash-on-change effect
  flashes: new Map(),    // "side:price" -> timestamp of last change
  tradesWindow: [],      // recent trade timestamps, for a rough trades/sec figure
};

const els = {
  conn: document.getElementById('conn'),
  seq: document.getElementById('seq'),
  tps: document.getElementById('tps'),
  mode: document.getElementById('mode'),
  ladderPanel: document.getElementById('ladder-panel'),
  histPanel: document.getElementById('hist-panel'),
  ladder: document.getElementById('ladder'),
  hist: document.getElementById('hist'),
};

function fmtPrice(p) { return (p / PRICE_SCALE).toFixed(2); }
function fmtQty(q) { return q.toLocaleString(); }
function fmtNs(ns) {
  if (ns < 1000) return ns + 'ns';
  if (ns < 1e6) return (ns / 1000).toFixed(1) + 'µs';
  if (ns < 1e9) return (ns / 1e6).toFixed(2) + 'ms';
  return (ns / 1e9).toFixed(2) + 's';
}

// --- WebSocket, reconnecting with backoff ------------------------------------------------------

let backoffMs = 250;
function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const ws = new WebSocket(`${proto}://${location.host}/`);

  ws.onopen = () => {
    backoffMs = 250;
    els.conn.textContent = 'connected';
    els.conn.className = 'pill pill-up';
  };
  ws.onmessage = (ev) => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    if (msg.t === 'book') {
      state.book = msg;
      state.lastBookAt = performance.now();
      els.seq.textContent = msg.seq;
      recordTrades(msg.trades);
    } else if (msg.t === 'lat') {
      state.lat = msg;
      state.lastLatAt = performance.now();
    }
  };
  ws.onclose = () => {
    els.conn.textContent = 'reconnecting…';
    els.conn.className = 'pill pill-down';
    setTimeout(connect, backoffMs);
    backoffMs = Math.min(backoffMs * 2, 5000);
  };
  ws.onerror = () => ws.close();
}
connect();

function recordTrades(trades) {
  if (!trades || !trades.length) return;
  const now = performance.now();
  // We don't get per-trade timestamps over the wire -- approximate trades/sec by counting how
  // many NEW trades arrived since the last book message and decaying a rolling window.
  const known = state.tradesWindow;
  const newCount = Math.max(0, (state.book.tradeCount || 0) - (known.lastCount || 0));
  for (let i = 0; i < newCount; ++i) known.push(now);
  known.lastCount = state.book.tradeCount;
  while (known.length && now - known[0] > 5000) known.shift();
}

// --- ladder ------------------------------------------------------------------------------------

function drawLadder() {
  const canvas = els.ladder;
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);

  const { bids, asks } = state.book;
  const rows = Math.max(bids.length, asks.length, 1);
  const rowH = Math.min(28, Math.floor((h - 10) / Math.max(rows, 12)));
  const midY = 6;
  const maxQty = Math.max(1, ...bids.map(l => l[1]), ...asks.map(l => l[1]));
  const now = performance.now();

  const colW = w / 2;
  drawSide(ctx, bids, 'bid', 0, colW, midY, rowH, maxQty, now);
  drawSide(ctx, asks, 'ask', colW, colW, midY, rowH, maxQty, now);

  // Center divider.
  ctx.strokeStyle = 'rgba(255,255,255,0.06)';
  ctx.beginPath();
  ctx.moveTo(colW, 0);
  ctx.lineTo(colW, h);
  ctx.stroke();
}

function drawSide(ctx, levels, side, x0, colW, y0, rowH, maxQty, now) {
  const style = getComputedStyle(document.documentElement);
  const barColor = side === 'bid' ? style.getPropertyValue('--bid') : style.getPropertyValue('--ask');
  const dimColor = side === 'bid' ? style.getPropertyValue('--bid-dim') : style.getPropertyValue('--ask-dim');

  ctx.font = '12px ui-monospace, monospace';
  ctx.textBaseline = 'middle';

  levels.forEach((lvl, i) => {
    const [price, qty, orders] = lvl;
    const y = y0 + i * rowH;
    const key = `${side}:${price}`;
    const prevQty = state.prevLevels.get(key);
    if (prevQty !== undefined && prevQty !== qty) {
      state.flashes.set(key, now);
    }
    state.prevLevels.set(key, qty);

    const flashAt = state.flashes.get(key);
    const flashAge = flashAt ? now - flashAt : Infinity;
    const flashAlpha = flashAge < 300 ? (1 - flashAge / 300) * 0.5 : 0;

    const barLen = Math.max(2, (qty / maxQty) * (colW - 90));
    ctx.fillStyle = dimColor;
    if (side === 'bid') {
      ctx.fillRect(x0 + colW - 90 - barLen, y + 2, barLen, rowH - 4);
    } else {
      ctx.fillRect(x0 + 90, y + 2, barLen, rowH - 4);
    }
    if (flashAlpha > 0) {
      ctx.fillStyle = barColor.trim();
      ctx.globalAlpha = flashAlpha;
      if (side === 'bid') {
        ctx.fillRect(x0 + colW - 90 - barLen, y + 2, barLen, rowH - 4);
      } else {
        ctx.fillRect(x0 + 90, y + 2, barLen, rowH - 4);
      }
      ctx.globalAlpha = 1;
    }

    ctx.fillStyle = barColor.trim();
    const priceText = fmtPrice(price);
    if (side === 'bid') {
      ctx.textAlign = 'right';
      ctx.fillText(priceText, x0 + colW - 6, y + rowH / 2);
    } else {
      ctx.textAlign = 'left';
      ctx.fillText(priceText, x0 + 6, y + rowH / 2);
    }

    ctx.fillStyle = '#8792a3';
    const qtyText = `${fmtQty(qty)} (${orders})`;
    if (side === 'bid') {
      ctx.textAlign = 'left';
      ctx.fillText(qtyText, x0 + 6, y + rowH / 2);
    } else {
      ctx.textAlign = 'right';
      ctx.fillText(qtyText, x0 + colW - 6, y + rowH / 2);
    }
  });
}

// --- latency histogram ---------------------------------------------------------------------
//
// The wire only carries four numbers per tick (p50/p99/p999/max -- telemetry/live_latency.hpp),
// not the full HdrHistogram distribution, so this draws a percentile-marker chart on a log-x
// axis (same visual language as benchmarks/plot_latency.py: log scale, budget lines at
// 2/20/100us) rather than a full density curve.

const BUDGETS_NS = [2000, 20000, 100000]; // 2us / 20us / 100us, from CLAUDE.md's hard gate
const AXIS_MIN_NS = 100;      // 100ns
const AXIS_MAX_NS = 2e9;      // 2s

function logX(ns, w) {
  const lo = Math.log10(AXIS_MIN_NS), hi = Math.log10(AXIS_MAX_NS);
  const t = (Math.log10(Math.max(ns, AXIS_MIN_NS)) - lo) / (hi - lo);
  return Math.min(1, Math.max(0, t)) * w;
}

function drawHist() {
  const canvas = els.hist;
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);

  const plotTop = 20, plotBottom = h - 90, plotH = plotBottom - plotTop;

  // Budget reference lines.
  ctx.strokeStyle = 'rgba(255,255,255,0.12)';
  ctx.setLineDash([4, 4]);
  ctx.font = '11px ui-monospace, monospace';
  ctx.fillStyle = '#6b7688';
  for (const b of BUDGETS_NS) {
    const x = logX(b, w);
    ctx.beginPath();
    ctx.moveTo(x, plotTop);
    ctx.lineTo(x, plotBottom);
    ctx.stroke();
    ctx.fillText(fmtNs(b), x + 3, plotTop + 10);
  }
  ctx.setLineDash([]);

  if (!state.lat) {
    ctx.fillStyle = '#6b7688';
    ctx.fillText('waiting for latency stream…', 10, plotTop + 30);
    return;
  }

  const style = getComputedStyle(document.documentElement);
  const bars = [
    { label: 'p50', ns: state.lat.p50_ns, color: style.getPropertyValue('--p50') },
    { label: 'p99', ns: state.lat.p99_ns, color: style.getPropertyValue('--p99') },
    { label: 'p999', ns: state.lat.p999_ns, color: style.getPropertyValue('--p999') },
    { label: 'max', ns: state.lat.max_ns, color: style.getPropertyValue('--max') },
  ];

  const barH = 26, gap = 14;
  bars.forEach((b, i) => {
    const y = plotTop + i * (barH + gap);
    const x = logX(b.ns, w);
    ctx.fillStyle = b.color.trim();
    ctx.fillRect(0, y, Math.max(2, x), barH);
    ctx.fillStyle = '#0b0e14';
    ctx.font = 'bold 12px ui-monospace, monospace';
    ctx.textBaseline = 'middle';
    ctx.textAlign = 'left';
    ctx.fillText(`${b.label}  ${fmtNs(b.ns)}`, 8, y + barH / 2);
  });

  ctx.fillStyle = '#8792a3';
  ctx.font = '12px ui-monospace, monospace';
  ctx.fillText(
    `n=${state.lat.count}  rate=${Math.round(state.lat.rate)}/s  (corrected, rolling ~1s window)`,
    0, plotBottom + 30);
}

// --- main loop -----------------------------------------------------------------------------

function tick() {
  const now = performance.now();
  const bookStale = now - state.lastBookAt > 3000;
  els.ladderPanel.classList.toggle('stale', bookStale && state.lastBookAt > 0);

  const known = state.tradesWindow;
  const recent = known.filter(t => now - t <= 5000).length;
  els.tps.textContent = (recent / 5).toFixed(1);

  drawLadder();
  drawHist();
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
