// SH1 frame debugger: decodes the GP0 streams captured by tools/port/compare_frames.py (PS1 from
// DuckStation, PS2 from the port), lists the draws with their geometry, and attributes the pixels
// that differ between the two final frames to the last draw that covered them ("blame").
"use strict";

const VRAM_W = 1024, VRAM_H = 512, SCALE = 3;
const $ = (id) => document.getElementById(id);
let cur = null;           // loaded frame
let mode = "ps1";         // image shown
let selected = -1;        // selected draw index

// --- Loading ---------------------------------------------------------------------------------------

// ?sync=1 loads with synchronous requests during startup (for headless screenshots, which are taken
// at the load event); ?frame=N and ?select=N pick a frame and a draw.
const PARAMS = new URLSearchParams(location.search);
const SYNC = PARAMS.has("sync");

function syncGet(url, binary) {
  const x = new XMLHttpRequest();
  x.open("GET", url, false);
  if (binary) x.overrideMimeType("text/plain; charset=x-user-defined");
  x.send();
  if (x.status !== 200) throw new Error(url + ": " + x.status);
  if (!binary) return x.responseText;
  const s = x.responseText, out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0xFF;
  return out;
}

async function bin(url) {
  if (SYNC) return syncGet(url, true);
  const r = await fetch(url);
  if (!r.ok) throw new Error(url + ": " + r.status);
  return new Uint8Array(await r.arrayBuffer());
}

async function text(url) {
  return SYNC ? syncGet(url, false) : fetch(url).then((r) => r.text());
}

function packets(bytes) {
  const dv = new DataView(bytes.buffer), out = [];
  let pos = 0;
  while (pos + 4 <= bytes.length) {
    const n = dv.getUint32(pos, true);
    const w = [];
    for (let i = 0; i < n; i++) w.push(dv.getUint32(pos + 4 + 4 * i, true));
    out.push(w);
    pos += 4 + 4 * n;
  }
  return out;
}

function vram(bytes) {
  return new Uint16Array(bytes.buffer, bytes.byteOffset, VRAM_W * VRAM_H);
}

async function loadFrame(n) {
  const [p1, p2, v1, v2, d1, d2] = await Promise.all([
    bin(`/cap/ps1_${n}_gp0.bin`), bin(`/cap/ps2_${n}_gp0.bin`),
    bin(`/cap/ps1_${n}_vram.bin`), bin(`/cap/ps2_${n}_vram.bin`),
    text(`/cap/ps1_${n}_disp.txt`), text(`/cap/ps2_${n}_disp.txt`),
  ]);
  const disp = d1.trim().split(/\s+/).slice(0, 4).map(Number);
  const disp2 = d2.trim().split(/\s+/).slice(0, 4).map(Number);
  // The PS2's displayed buffer can be the other one of the double buffer: its pixel for a PS1
  // display position is at the same offset in its own displayed area.
  const f = { n, disp, disp2, vram1: vram(v1), vram2: vram(v2) };
  f.at2 = (i) => {
    const x = i & 1023, y = i >> 10;
    return f.vram2[((y - disp[1] + disp2[1]) & 511) * VRAM_W + ((x - disp[0] + disp2[0]) & 1023)];
  };
  f.draws1 = decode(packets(p1));
  f.draws2 = decode(packets(p2));
  f.owner = coverage(f.draws1);
  blame(f);
  return f;
}

// --- GP0 decoding (as src/port/gpu_soft.c) ----------------------------------------------------------

const sext11 = (v) => ((v & 0x7FF) ^ 0x400) - 0x400;

function decode(pkts) {
  const st = { texPage: 0, tw: [0, 0, 0, 0], area: [0, 0, VRAM_W - 1, VRAM_H - 1], ofs: [0, 0], setMask: 0, checkMask: 0 };
  const draws = [];
  pkts.forEach((w, pi) => {
    let k = 0;
    while (k < w.length) {
      const cmd = w[k] >>> 24;
      const rest = w.slice(k);
      let used = 1;
      if (cmd >= 0x20 && cmd < 0x40) used = polygon(rest, st, draws, pi);
      else if (cmd >= 0x40 && cmd < 0x60) used = lines(rest, st, draws, pi);
      else if (cmd >= 0x60 && cmd < 0x80) used = rect(rest, st, draws, pi);
      else if (cmd === 0x02) {
        draws.push({ type: "fill", packet: pi, words: rest.slice(0, 3), color: rest[0] & 0xFFFFFF,
          x: rest[1] & 0x3F0, y: (rest[1] >>> 16) & 0x1FF, w: (((rest[2] & 0x3FF) + 15) & ~15), h: (rest[2] >>> 16) & 0x1FF });
        used = 3;
      } else if (cmd >= 0x80 && cmd < 0xA0) {
        draws.push({ type: "vram copy", packet: pi, words: rest.slice(0, 4) });
        used = 4;
      } else if (cmd >= 0xE1 && cmd <= 0xE6) settings(w[k], st);
      k += Math.max(1, used);
    }
  });
  draws.forEach((d, i) => { d.index = i; });
  return draws;
}

function settings(w, st) {
  switch (w >>> 24) {
    case 0xE1: st.texPage = w & 0x3FFF; break;
    case 0xE2: st.tw = [w & 31, (w >>> 5) & 31, (w >>> 10) & 31, (w >>> 15) & 31]; break;
    case 0xE3: st.area = [w & 0x3FF, (w >>> 10) & 0x1FF, st.area[2], st.area[3]]; break;
    case 0xE4: st.area = [st.area[0], st.area[1], w & 0x3FF, (w >>> 10) & 0x1FF]; break;
    case 0xE5: st.ofs = [sext11(w), sext11(w >>> 11)]; break;
    case 0xE6: st.setMask = w & 1; st.checkMask = (w >>> 1) & 1; break;
  }
}

function common(st) {
  return { area: st.area.slice(), ofs: st.ofs.slice(), tw: st.tw.slice(), setMask: st.setMask, checkMask: st.checkMask };
}

function polygon(w, st, draws, pi) {
  const cmd = w[0] >>> 24, gouraud = (cmd >> 4) & 1, textured = (cmd >> 2) & 1, raw = (cmd & 1) && textured;
  const semi = (cmd >> 1) & 1, nv = cmd & 8 ? 4 : 3;
  const v = [];
  let k = 0, tpage = st.texPage, clut = 0;
  for (let i = 0; i < nv; i++) {
    const vt = {};
    if (i === 0 || gouraud) { const c = w[k++]; vt.r = c & 0xFF; vt.g = (c >>> 8) & 0xFF; vt.b = (c >>> 16) & 0xFF; }
    else { vt.r = v[0].r; vt.g = v[0].g; vt.b = v[0].b; }
    const xy = w[k++];
    vt.x = sext11(xy) + st.ofs[0];
    vt.y = sext11(xy >>> 16) + st.ofs[1];
    if (textured) {
      const uv = w[k++];
      vt.u = uv & 0xFF; vt.v = (uv >>> 8) & 0xFF;
      if (i === 0) clut = uv >>> 16;
      if (i === 1) tpage = (st.texPage & ~0x1FF) | ((uv >>> 16) & 0x1FF);
    }
    v.push(vt);
  }
  if (textured) st.texPage = tpage;
  draws.push(Object.assign(common(st), {
    type: `${nv === 4 ? "quad" : "tri"}${gouraud ? " gouraud" : " flat"}${textured ? (raw ? " tex-raw" : " tex") : ""}${semi ? " semi" : ""}`,
    packet: pi, words: w.slice(0, k), verts: v, prim: nv === 4 ? [[0, 1, 2], [1, 2, 3]] : [[0, 1, 2]],
    textured, gouraud, semi, raw, tpage, clut, semiMode: (tpage >> 5) & 3,
    dither: ((st.texPage >> 9) & 1) && (gouraud || (textured && !raw)),
  }));
  return k;
}

function rect(w, st, draws, pi) {
  const cmd = w[0] >>> 24, siz = (cmd >> 3) & 3, textured = (cmd >> 2) & 1, semi = (cmd >> 1) & 1;
  let k = 0;
  const c = w[k++], xy = w[k++];
  const x = sext11(xy) + st.ofs[0], y = sext11(xy >>> 16) + st.ofs[1];
  let u = 0, vv = 0, clut = 0;
  if (textured) { const uv = w[k++]; u = uv & 0xFF; vv = (uv >>> 8) & 0xFF; clut = uv >>> 16; }
  let rw, rh;
  if (siz === 0) { rw = w[k] & 0x3FF; rh = (w[k] >>> 16) & 0x1FF; k++; }
  else rw = rh = siz === 1 ? 1 : siz === 2 ? 8 : 16;
  draws.push(Object.assign(common(st), {
    type: `rect${textured ? " tex" : ""}${semi ? " semi" : ""}`, packet: pi, words: w.slice(0, k),
    verts: [{ x, y, r: c & 0xFF, g: (c >>> 8) & 0xFF, b: (c >>> 16) & 0xFF, u, v: vv }], w: rw, h: rh,
    textured, semi, tpage: st.texPage, clut, semiMode: (st.texPage >> 5) & 3,
  }));
  return k;
}

function lines(w, st, draws, pi) {
  const cmd = w[0] >>> 24, gouraud = (cmd >> 4) & 1, poly = cmd & 8, semi = (cmd >> 1) & 1;
  let k = 0;
  const pts = [];
  let c = w[k++];
  for (;;) {
    if (k >= w.length) break;
    if (pts.length && poly && (w[k] & 0xF000F000) === 0x50005000) { k++; break; }
    if (pts.length && gouraud) c = w[k++];
    if (k >= w.length) break;
    const xy = w[k++];
    pts.push({ x: sext11(xy) + st.ofs[0], y: sext11(xy >>> 16) + st.ofs[1], r: c & 0xFF, g: (c >>> 8) & 0xFF, b: (c >>> 16) & 0xFF });
    if (!poly && pts.length === 2) break;
  }
  draws.push(Object.assign(common(st), { type: `line${poly ? " poly" : ""}${semi ? " semi" : ""}`, packet: pi, words: w.slice(0, k), verts: pts, semi }));
  return k;
}

// --- Coverage (which draw wrote each pixel last) --------------------------------------------------

function coverage(draws) {
  const owner = new Int32Array(VRAM_W * VRAM_H).fill(-1);
  for (const d of draws) {
    const [x1, y1, x2, y2] = d.area || [0, 0, VRAM_W - 1, VRAM_H - 1];
    const put = (x, y) => {
      if (x >= x1 && x <= x2 && y >= y1 && y <= y2 && x >= 0 && y >= 0 && x < VRAM_W && y < VRAM_H) owner[y * VRAM_W + x] = d.index;
    };
    if (d.type === "fill") {
      for (let y = d.y; y < d.y + d.h; y++) for (let x = d.x; x < d.x + d.w; x++) owner[(y & 511) * VRAM_W + (x & 1023)] = d.index;
    } else if (d.type.startsWith("rect")) {
      const v = d.verts[0];
      for (let y = v.y; y < v.y + d.h; y++) for (let x = v.x; x < v.x + d.w; x++) put(x, y);
    } else if (d.type.startsWith("line")) {
      for (let i = 0; i + 1 < d.verts.length; i++) line(d.verts[i], d.verts[i + 1], put);
    } else if (d.prim) {
      for (const t of d.prim) triangle(d.verts[t[0]], d.verts[t[1]], d.verts[t[2]], put);
    }
  }
  return owner;
}

function triangle(a, b, c, put) {
  const span = (p, q) => Math.abs(p - q);
  if (span(a.x, b.x) > 1023 || span(b.x, c.x) > 1023 || span(a.x, c.x) > 1023 ||
      span(a.y, b.y) > 511 || span(b.y, c.y) > 511 || span(a.y, c.y) > 511) return;
  const edge = (p, q, x, y) => (q.x - p.x) * (y - p.y) - (q.y - p.y) * (x - p.x);
  let area = edge(a, b, c.x, c.y);
  if (area === 0) return;
  if (area < 0) [b, c] = [c, b];
  const tl = (p, q) => (p.y === q.y && q.x < p.x) || q.y < p.y;
  const t0 = tl(b, c), t1 = tl(c, a), t2 = tl(a, b);
  const minx = Math.min(a.x, b.x, c.x), maxx = Math.max(a.x, b.x, c.x);
  const miny = Math.min(a.y, b.y, c.y), maxy = Math.max(a.y, b.y, c.y);
  for (let y = miny; y <= maxy; y++) {
    for (let x = minx; x <= maxx; x++) {
      const w0 = edge(b, c, x, y), w1 = edge(c, a, x, y), w2 = edge(a, b, x, y);
      if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 === 0 && !t0) || (w1 === 0 && !t1) || (w2 === 0 && !t2)) continue;
      put(x, y);
    }
  }
}

function line(a, b, put) {
  const dx = b.x - a.x, dy = b.y - a.y, steps = Math.max(Math.abs(dx), Math.abs(dy));
  if (Math.abs(dx) > 1023 || Math.abs(dy) > 511) return;
  for (let i = 0; i <= steps; i++) {
    put(steps ? a.x + Math.round((dx * i) / steps) : a.x, steps ? a.y + Math.round((dy * i) / steps) : a.y);
  }
}

// --- Comparison -----------------------------------------------------------------------------------

function normalize(words) {
  const cmd = words[0] >>> 24;
  if (cmd >= 0x20 && cmd < 0x40 && (cmd & 4)) {
    const g = cmd & 0x10, nv = cmd & 8 ? 4 : 3, w = words.slice();
    for (let i = 2; i < nv; i++) { const k = g ? 2 + i * 3 : 2 + i * 2; if (k < w.length) w[k] &= 0xFFFF; }
    return w.join(",");
  }
  return words.join(",");
}

function blame(f) {
  const [dx, dy, dw, dh] = f.disp;
  const counts = new Map();
  let total = 0;
  for (let y = dy; y < dy + dh; y++) {
    for (let x = dx; x < dx + dw; x++) {
      const i = (y & 511) * VRAM_W + (x & 1023);
      if ((f.vram1[i] ^ f.at2(i)) & 0x7FFF) {
        total++;
        const o = f.owner[i];
        counts.set(o, (counts.get(o) || 0) + 1);
      }
    }
  }
  f.diffTotal = total;
  f.blame = [...counts.entries()].sort((a, b) => b[1] - a[1]);
  f.streamDiff = new Set();
  const n = Math.max(f.draws1.length, f.draws2.length);
  for (let i = 0; i < n; i++) {
    const a = f.draws1[i], b = f.draws2[i];
    if (!a || !b || normalize(a.words) !== normalize(b.words)) f.streamDiff.add(i);
  }
}

// --- Views ----------------------------------------------------------------------------------------

function rgb(c) { return [(c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3]; }

function render() {
  if (!cur) return;
  const [dx, dy, dw, dh] = cur.disp;
  const cv = $("view");
  cv.width = dw * SCALE;
  cv.height = dh * SCALE;
  const ctx = cv.getContext("2d");
  const img = ctx.createImageData(dw, dh);
  const blamed = $("showBlame").checked && selected >= 0;
  for (let y = 0; y < dh; y++) {
    for (let x = 0; x < dw; x++) {
      const i = ((dy + y) & 511) * VRAM_W + ((dx + x) & 1023), o = (y * dw + x) * 4;
      const a = cur.vram1[i], b = cur.at2(i), differs = (a ^ b) & 0x7FFF;
      let c;
      if (mode === "diff") c = differs ? [255, 255, 255] : rgb(a).map((v) => v >> 2);
      else c = rgb(mode === "ps1" ? a : b);
      if (blamed && differs && cur.owner[i] === selected) c = [255, 60, 0];
      img.data[o] = c[0]; img.data[o + 1] = c[1]; img.data[o + 2] = c[2]; img.data[o + 3] = 255;
    }
  }
  const tmp = document.createElement("canvas");
  tmp.width = dw; tmp.height = dh;
  tmp.getContext("2d").putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(tmp, 0, 0, dw * SCALE, dh * SCALE);

  const outline = (d, style, width) => {
    ctx.strokeStyle = style;
    ctx.lineWidth = width;
    const X = (v) => (v.x - dx + 0.5) * SCALE, Y = (v) => (v.y - dy + 0.5) * SCALE;
    ctx.beginPath();
    if (d.prim) {
      for (const t of d.prim) {
        ctx.moveTo(X(d.verts[t[0]]), Y(d.verts[t[0]]));
        ctx.lineTo(X(d.verts[t[1]]), Y(d.verts[t[1]]));
        ctx.lineTo(X(d.verts[t[2]]), Y(d.verts[t[2]]));
        ctx.closePath();
      }
    } else if (d.type.startsWith("rect")) {
      const v = d.verts[0];
      ctx.rect((v.x - dx) * SCALE, (v.y - dy) * SCALE, d.w * SCALE, d.h * SCALE);
    } else if (d.type.startsWith("line")) {
      d.verts.forEach((v, i) => (i ? ctx.lineTo(X(v), Y(v)) : ctx.moveTo(X(v), Y(v))));
    }
    ctx.stroke();
  };
  if ($("outlineAll").checked) cur.draws1.forEach((d) => d.verts && outline(d, "rgba(37,99,235,0.35)", 1));
  if (selected >= 0 && cur.draws1[selected] && cur.draws1[selected].verts) outline(cur.draws1[selected], "#ffcc00", 2);
}

function hex(words) { return words.map((v) => (v >>> 0).toString(16).toUpperCase().padStart(8, "0")).join(" "); }

function describe(d) {
  if (!d) return "(none)";
  const lines = [`#${d.index}  ${d.type}   (packet ${d.packet})`];
  if (d.verts) {
    d.verts.forEach((v, i) => lines.push(`  v${i}: x=${v.x} y=${v.y}  rgb=${v.r},${v.g},${v.b}` + (v.u !== undefined ? `  uv=${v.u},${v.v}` : "")));
  }
  if (d.w !== undefined && d.type.startsWith("rect")) lines.push(`  size ${d.w}x${d.h}`);
  if (d.textured) {
    const depth = ["4-bit", "8-bit", "15-bit", "15-bit"][(d.tpage >> 7) & 3];
    lines.push(`  texture page x=${(d.tpage & 15) * 64} y=${((d.tpage >> 4) & 1) * 256} ${depth}` +
      (((d.tpage >> 7) & 3) < 2 ? `  CLUT x=${(d.clut & 63) * 16} y=${(d.clut >> 6) & 511}` : "") + (d.raw ? "  raw" : "  modulated"));
  }
  if (d.semi) lines.push(`  semi-transparent mode ${d.semiMode} (${["B/2+F/2", "B+F", "B-F", "B+F/4"][d.semiMode]})`);
  if (d.dither) lines.push("  dithered");
  if (d.area) lines.push(`  clip ${d.area[0]},${d.area[1]}-${d.area[2]},${d.area[3]}  offset ${d.ofs[0]},${d.ofs[1]}` +
    (d.tw.some((x) => x) ? `  texture window ${d.tw}` : "") + (d.setMask ? "  set-mask" : "") + (d.checkMask ? "  check-mask" : ""));
  lines.push(`  words: ${hex(d.words)}`);
  return lines.join("\n");
}

function select(i) {
  selected = i;
  const a = cur.draws1[i], b = cur.draws2[i];
  let text = "PS1:\n" + describe(a) + "\n\nPS2:\n" + describe(b);
  if (cur.streamDiff.has(i)) text = "COMMANDS DIFFER\n\n" + text;
  const blamedPx = (cur.blame.find((e) => e[0] === i) || [0, 0])[1];
  text += `\n\nDiffering pixels last written by this draw: ${blamedPx}`;
  $("detail").textContent = text;
  document.querySelectorAll("tr.sel").forEach((r) => r.classList.remove("sel"));
  document.querySelectorAll(`tr[data-i="${i}"]`).forEach((r) => r.classList.add("sel"));
  render();
}

function touchesDisplay(d) {
  if (!d.verts) return d.type === "fill";
  const [dx, dy, dw, dh] = cur.disp;
  const xs = d.verts.map((v) => v.x), ys = d.verts.map((v) => v.y);
  const x1 = Math.min(...xs), y1 = Math.min(...ys), x2 = Math.max(...xs) + (d.w || 0), y2 = Math.max(...ys) + (d.h || 0);
  return x2 >= dx && x1 < dx + dw && y2 >= dy && y1 < dy + dh;
}

function fillTables() {
  const onlyDiff = $("onlyDiff").checked, onlyVisible = $("onlyVisible").checked;
  const rows = [];
  for (const d of cur.draws1) {
    if (onlyDiff && !cur.streamDiff.has(d.index)) continue;
    if (onlyVisible && !touchesDisplay(d)) continue;
    const verts = d.verts ? d.verts.map((v) => `${v.x},${v.y}`).join(" ") : "";
    const tex = d.textured ? `${d.tpage.toString(16)}/${d.clut.toString(16)}` : "";
    const flags = [d.semi ? "semi" + d.semiMode : "", d.dither ? "dith" : "", d.setMask ? "set" : "", d.checkMask ? "chk" : ""].filter(Boolean).join(" ");
    rows.push(`<tr data-i="${d.index}" class="${cur.streamDiff.has(d.index) ? "diff" : ""}"><td>${d.index}</td><td>${d.type}</td><td>${verts}</td><td>${tex}</td><td>${flags}</td></tr>`);
  }
  $("draws").innerHTML = rows.join("");
  $("drawCount").textContent = `(${rows.length} shown of ${cur.draws1.length} PS1, ${cur.draws2.length} PS2)`;
  $("blame").innerHTML = cur.blame.slice(0, 200).map(([i, c]) =>
    i < 0 ? `<tr><td>none</td><td>${c}</td><td colspan="2">not drawn this frame</td></tr>`
          : `<tr data-i="${i}"><td>${i}</td><td>${c}</td><td>${cur.draws1[i].type}</td><td>${cur.streamDiff.has(i) ? '<span class="tag">differs</span>' : "same"}</td></tr>`).join("");
  document.querySelectorAll("tbody tr[data-i]").forEach((r) => r.addEventListener("click", () => select(Number(r.dataset.i))));
}

async function show(n) {
  $("summary").textContent = "loading…";
  cur = await loadFrame(n);
  selected = -1;
  const [, , dw, dh] = cur.disp;
  $("summary").textContent = `${cur.draws1.length} draws (PS1), ${cur.draws2.length} (PS2); ` +
    `${cur.streamDiff.size} draws with different commands; ${cur.diffTotal} of ${dw * dh} display pixels differ`;
  fillTables();
  $("detail").textContent = "Click a draw in a list, or a pixel in the image (its last draw).";
  render();
}

// --- Wiring ---------------------------------------------------------------------------------------

document.querySelectorAll("button[data-img]").forEach((b) => b.addEventListener("click", () => {
  mode = b.dataset.img;
  document.querySelectorAll("button[data-img]").forEach((x) => x.classList.toggle("on", x === b));
  render();
}));
["outlineAll", "showBlame"].forEach((id) => $(id).addEventListener("change", render));
["onlyDiff", "onlyVisible"].forEach((id) => $(id).addEventListener("change", fillTables));
$("frame").addEventListener("change", (e) => show(Number(e.target.value)));

function pixelAt(ev) {
  const r = $("view").getBoundingClientRect();
  const [dx, dy, dw, dh] = cur.disp;
  const x = Math.floor(((ev.clientX - r.left) / r.width) * dw), y = Math.floor(((ev.clientY - r.top) / r.height) * dh);
  return [x + dx, y + dy];
}
$("view").addEventListener("mousemove", (ev) => {
  if (!cur) return;
  const [x, y] = pixelAt(ev), i = (y & 511) * VRAM_W + (x & 1023);
  const f = (c) => rgb(c).map((v) => v >> 3).join(",");
  $("hover").textContent = `x=${x} y=${y}  PS1 ${f(cur.vram1[i])}  PS2 ${f(cur.at2(i))} (5-bit)  last draw ${cur.owner[i]}`;
});
$("view").addEventListener("click", (ev) => {
  if (!cur) return;
  const [x, y] = pixelAt(ev), o = cur.owner[(y & 511) * VRAM_W + (x & 1023)];
  if (o >= 0) select(o);
});

function start(frames) {
  $("frame").innerHTML = frames.map((n) => `<option value="${n}">${n}</option>`).join("");
  if (!frames.length) {
    $("summary").textContent = "No captures: run tools/port/compare_frames.py first.";
    return;
  }
  const want = PARAMS.has("frame") ? Number(PARAMS.get("frame")) : frames[frames.length - 1];
  $("frame").value = String(want);
  const done = show(want).then(() => { if (PARAMS.has("select")) select(Number(PARAMS.get("select"))); });
  done.catch((e) => { $("summary").textContent = "error: " + e; });
}

if (SYNC) {
  try { start(JSON.parse(syncGet("/frames.json", false))); } catch (e) { $("summary").textContent = "error: " + e; }
} else {
  fetch("/frames.json").then((r) => r.json()).then(start);
}
