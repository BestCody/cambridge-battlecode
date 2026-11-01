// Realistic-map generator for GA training (the edge maps cover the extremes).
// Terrain comes from the pieces real maps are made of: straight wall lines
// with doors (open or bush), hooks, rooms, stair-step bands, pillar rows,
// hole rivers and pools, bush strips beside walls, bush patches and frames
// round altars. Camp groups are the GA-half real maps' groups (kinds and
// shape, turned and flipped) put in new places, sometimes one against the
// mirror line where it merges into a contested middle group. Altars sit
// mostly towards the middle. A map has 18-140 camps, as real maps do whatever
// their size; half the maps are dense, half sparse: at least / under 32 camps
// per 1000 walkable tiles (the GA half's median is about 30). The builder
// mirrors every placement; the checks run on the full map.
// usage: node genreal.mjs <engine.wasm> <realstats.json> <out dir> <maps per class>
import fs from "node:fs";
import { loadEngine, CAMP } from "./maplib.mjs";

const [wasm, statsPath, outDir, perClassArg] = process.argv.slice(2);
const PER_CLASS = Number(perClassArg ?? 50);
const B = await loadEngine(wasm);
fs.mkdirSync(outDir, { recursive: true });

// camp-group templates: every GA-half group of up to 30 camps, [kind, dx, dy]
const real = JSON.parse(fs.readFileSync(statsPath, "utf8"));
const TEMPLATES = real.flatMap((m) => m.groups).filter((g) => g.n <= 30)
  .map((g) => g.cells.map(([k, dx, dy]) => [CAMP[k], dx, dy]));
const FIELDS = TEMPLATES.filter((t) => t.length >= 8);
const CENTRE = TEMPLATES.filter((t) => t.length >= 3);
const BOSSES = CENTRE.filter((t) => t.some(([k]) => k === CAMP.monolith || k === CAMP.golem));

function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const hash = (s) => [...s].reduce((h, c) => (Math.imul(h, 31) + c.charCodeAt(0)) | 0, 7);
const pickW = (r, list) => {
  const total = list.reduce((s, [, w]) => s + w, 0);
  let u = r() * total;
  for (const [v, w] of list) if ((u -= w) < 0) return v;
  return list[list.length - 1][0];
};
const cheb = (a, b) => Math.max(Math.abs(a[0] - b[0]), Math.abs(a[1] - b[1]));
const N8 = [[-1, -1], [0, -1], [1, -1], [-1, 0], [1, 0], [-1, 1], [0, 1], [1, 1]];

function build(cls, seed) {
  const r = rng(seed);
  const ri = (a, b) => a + Math.floor(r() * (b - a + 1));
  const any = (list) => list[Math.floor(r() * list.length)];

  // size, symmetry, champions, spawn
  const size = pickW(r, [["tiny", 25], ["small", 25], ["medium", 35], ["large", 15]]);
  let W = size === "tiny" ? ri(29, 33) : size === "small" ? ri(34, 45) : size === "medium" ? ri(46, 65) : ri(66, 100);
  let H = r() < 0.55 ? W : Math.max(29, Math.min(100, Math.round(W * (0.6 + r() * 1.1))));
  const sym = pickW(r, [[2, 50], [0, 25], [1, 25]]);
  const champs = W * H < 1300 ? pickW(r, [[3, 40], [4, 35], [5, 25]]) : W * H < 3000 ? pickW(r, [[3, 15], [4, 30], [5, 55]])
               : pickW(r, [[4, 25], [5, 75]]);
  const centreAltar = r() < 0.25;
  const odd = (n) => (n % 2 ? n : n === 100 ? 99 : n + 1);
  if (centreAltar) { if (sym !== 1) W = odd(W); if (sym !== 0) H = odd(H); }
  const idx = ([x, y]) => y * W + x;
  const mirror = ([x, y]) => (sym === 0 ? [W - 1 - x, y] : sym === 1 ? [x, H - 1 - y] : [W - 1 - x, H - 1 - y]);
  const inMap = ([x, y]) => x >= 0 && y >= 0 && x < W && y < H;
  const lineDist = ([x, y]) => (sym === 1 ? (H - 1) / 2 - y : (W - 1) / 2 - x);
  const T = Array.from({ length: H }, () => new Array(W).fill("."));
  const get = (p) => T[p[1]][p[0]];
  const counts = { "#": 0, o: 0, '"': 0 };
  const set = (p, ch) => {
    const m = mirror(p), n = idx(p) === idx(m) ? 1 : 2;
    if (counts[get(p)] !== undefined) counts[get(p)] -= n;
    if (counts[ch] !== undefined) counts[ch] += n;
    T[p[1]][p[0]] = ch; T[m[1]][m[0]] = ch;
  };
  const reserved = new Uint8Array(W * H);
  const reserve = (p, d) => {
    for (let dx = -d; dx <= d; dx++) for (let dy = -d; dy <= d; dy++) {
      const q = [p[0] + dx, p[1] + dy];
      if (inMap(q)) { reserved[idx(q)] = 1; reserved[idx(mirror(q))] = 1; }
    }
  };
  const paint = (p, ch) => {
    if (!inMap(p) || reserved[idx(p)] || get(p) !== ".") return false;
    set(p, ch); return true;
  };
  const shareOf = (ch) => counts[ch] / (W * H);

  // spawn: a along the attack axis (small = own side), b across it
  const sw = pickW(r, [[4, 75], [5, 25]]);
  const spawnStyle = pickW(r, [["corner", 47], ["edge", 33], ["inner", 20]]);
  const along = sym === 1 ? H : W, across = sym === 1 ? W : H, off = ri(1, 3);
  let a, b;
  if (spawnStyle === "inner") { // up to near the middle, as some real maps do (spawns a few tiles apart)
    a = ri(Math.floor(along * 0.2), Math.floor((along - 1) / 2) - 1 - sw);
    b = ri(off + 2, across - off - sw - 2);
  }
  else if (spawnStyle === "corner") { a = off; b = r() < 0.5 ? off : across - off - sw; }
  else { a = off; b = Math.floor(across / 2) - Math.floor(sw / 2) + ri(-Math.floor(across / 8), Math.floor(across / 8)); }
  if (sym === 2 && spawnStyle === "edge" && r() < 0.5) { // top or bottom edge of the left half
    b = r() < 0.5 ? off : across - off - sw;
    a = ri(Math.floor(along * 0.15), Math.floor(along / 2) - sw - 3);
  }
  const [sx, sy] = sym === 1 ? [b, a] : [a, b];
  const spawn = [];
  for (let dx = 0; dx < sw; dx++) for (let dy = 0; dy < sw; dy++) spawn.push([sx + dx, sy + dy]);
  for (const p of spawn) { set(p, "S"); reserve(p, 2); }
  const spawnB = spawn.map(mirror);
  const cA = [sx + (sw - 1) / 2, sy + (sw - 1) / 2], cB = mirror(cA);
  const dist = (p, q) => Math.hypot(p[0] - q[0], p[1] - q[1]);
  const between = (p) => dist(p, cA) / (dist(p, cA) + dist(p, cB)); // 0 own spawn, 0.5 middle
  const nearSpawn = (p, d) => spawn.some((s) => cheb(s, p) <= d) || spawnB.some((s) => cheb(s, p) <= d);

  // terrain amounts (shares of all tiles) and pieces
  const jitter = () => 0.7 + 0.6 * r();
  const wallT = pickW(r, [[0, 15], [0.03, 25], [0.07, 30], [0.12, 20], [0.17, 10]]) * jitter();
  const holeT = pickW(r, [[0, 40], [0.03, 22], [0.08, 18], [0.15, 10], [0.25, 6], [0.4, 4]]) * jitter();
  const bushT = pickW(r, [[0, 8], [0.015, 30], [0.03, 35], [0.05, 17], [0.09, 10]]) * jitter();
  const alongBias = r(); // share of straight pieces running along the attack axis
  const rand = () => [ri(0, W - 1), ri(0, H - 1)];
  const line = (ch) => {
    const alongAxis = r() < alongBias;
    const dir = (sym === 1) === alongAxis ? 1 : 0; // 0 = horizontal
    const len = ri(6, Math.max(7, Math.round((dir === 0 ? W : H) * (0.3 + 0.45 * r()))));
    const thick = ch === "#" ? pickW(r, [[1, 75], [2, 25]]) : pickW(r, [[1, 35], [2, 40], [3, 25]]);
    const p = rand(), doors = new Set();
    if (len >= 8 && r() < 0.6) for (let n = ri(1, 2); n > 0; n--) {
      const at = ri(1, len - 4), w = ri(2, 3);
      for (let k = at; k < at + w; k++) doors.add(k);
    }
    const doorCh = ch === "#" && r() < 0.35 ? '"' : null;
    for (let k = 0; k < len; k++) for (let t = 0; t < thick; t++) {
      const q = dir === 0 ? [p[0] + k, p[1] + t] : [p[0] + t, p[1] + k];
      if (!doors.has(k)) paint(q, ch); else if (doorCh) paint(q, doorCh);
    }
    if (ch === "#" && r() < 0.3) { // bush strip along one side
      const side = r() < 0.5 ? -1 : thick;
      for (let k = 0; k < len; k++) paint(dir === 0 ? [p[0] + k, p[1] + side] : [p[0] + side, p[1] + k], '"');
    }
  };
  const hook = () => {
    const p = rand(), la = ri(6, 14), lb = ri(4, 10), dx = r() < 0.5 ? 1 : -1, dy = r() < 0.5 ? 1 : -1;
    for (let k = 0; k < la; k++) paint([p[0] + dx * k, p[1]], "#");
    for (let k = 0; k < lb; k++) paint([p[0] + dx * (la - 1), p[1] + dy * k], "#");
    if (r() < 0.4) for (let k = 0; k < lb; k++) paint([p[0], p[1] + dy * k], "#"); // U shape
  };
  const room = () => {
    const w = ri(6, 14), h = ri(6, 14), p = rand(), open = new Set();
    for (let n = ri(1, 3); n > 0; n--) {
      const side = ri(0, 3), wd = ri(2, 3), len = side < 2 ? w : h, at = ri(1, Math.max(1, len - 1 - wd));
      for (let k = at; k < at + wd; k++)
        open.add(String(side === 0 ? [p[0] + k, p[1]] : side === 1 ? [p[0] + k, p[1] + h - 1] : side === 2 ? [p[0], p[1] + k] : [p[0] + w - 1, p[1] + k]));
    }
    const ring = [];
    for (let x = 0; x < w; x++) ring.push([p[0] + x, p[1]], [p[0] + x, p[1] + h - 1]);
    for (let y = 1; y < h - 1; y++) ring.push([p[0], p[1] + y], [p[0] + w - 1, p[1] + y]);
    for (const q of ring) if (!open.has(String(q))) paint(q, "#");
  };
  const band = (ch) => { // stair-step diagonal band, at least 2 wide so it cannot be cut diagonally
    const p = rand(), n = ri(6, Math.max(7, Math.round(Math.min(W, H) * 0.6))), dx = r() < 0.5 ? 1 : -1, dy = r() < 0.5 ? 1 : -1;
    const wd = ch === "o" ? ri(2, 4) : ri(2, 3);
    for (let k = 0; k < n; k++) for (let t = 0; t < wd; t++) paint([p[0] + dx * (k + t), p[1] + dy * k], ch);
  };
  const pillars = () => {
    const p = rand(), gap = ri(3, 6), nx = ri(2, 5), ny = ri(1, 4), s = pickW(r, [[1, 60], [2, 40]]);
    for (let i = 0; i < nx; i++) for (let j = 0; j < ny; j++)
      for (let dx = 0; dx < s; dx++) for (let dy = 0; dy < s; dy++) paint([p[0] + i * gap + dx, p[1] + j * gap + dy], "#");
  };
  const pool = (ch, rmax) => {
    const c = rand(), rx = ri(1, rmax), ry = Math.max(1, rx + ri(-1, 1));
    for (let dx = -rx; dx <= rx; dx++) for (let dy = -ry; dy <= ry; dy++)
      if ((dx * dx) / (rx * rx + 0.5) + (dy * dy) / (ry * ry + 0.5) <= 1 + 0.25 * (r() - 0.5)) paint([c[0] + dx, c[1] + dy], ch);
  };
  const fill = (ch, target, makers) => {
    for (let tries = 0; tries < 600 && shareOf(ch) < target; tries++) pickW(r, makers)();
  };
  fill("o", holeT, [[() => line("o"), 35], [() => band("o"), 25], [() => pool("o", 6), 40]]);
  fill("#", wallT, [[() => line("#"), 50], [hook, 15], [room, 15], [() => band("#"), 12], [pillars, 5], [() => pool("#", 2), 3]]);
  fill('"', bushT, [[() => pool('"', 4), 60], [() => line('"'), 25], [() => band('"'), 15]]);

  // altars: maybe one on the mirror line's centre, plus 0-3 per half, mostly towards the middle
  const altars = [];
  const putAltar = (p) => { set(p, "A"); altars.push(p); reserve(p, 1); };
  if (centreAltar) {
    const cx = (W - 1) / 2, cy = (H - 1) / 2;
    const p = sym === 0 ? [cx, Math.round(cy) + ri(-Math.floor(H / 6), Math.floor(H / 6))]
            : sym === 1 ? [Math.round(cx) + ri(-Math.floor(W / 6), Math.floor(W / 6)), cy] : [cx, cy];
    for (const [dx, dy] of [[0, 0], ...N8]) { const q = [p[0] + dx, p[1] + dy]; if (inMap(q) && get(q) !== "S") set(q, "."); }
    putAltar(p);
  }
  const perHalf = centreAltar ? pickW(r, [[0, 55], [1, 35], [2, 10]]) : pickW(r, [[1, 65], [2, 27], [3, 8]]);
  for (let n = 0, tries = 0; n < perHalf && tries < 4000; tries++) {
    const [lo, hi] = r() < 0.7 ? [0.36, 0.49] : [0.18, 0.36];
    const p = rand();
    if (lineDist(p) < 1 || get(p) !== "." || reserved[idx(p)] || nearSpawn(p, 4)) continue;
    const s = between(p);
    if (s < lo || s > hi || altars.some((q) => cheb(p, q) < 4 || cheb(mirror(p), q) < 4)) continue;
    putAltar(p); n++;
  }
  if (!altars.length) return { ok: false, why: "no altar" };
  for (const p of altars) { // frames: a bush ring with holes, or a wall ring open on one side
    const deco = pickW(r, [["none", 55], ["bush", 30], ["wall", 15]]);
    if (deco === "none") continue;
    const openSide = ri(0, 3);
    for (let dx = -2; dx <= 2; dx++) for (let dy = -2; dy <= 2; dy++) {
      if (Math.max(Math.abs(dx), Math.abs(dy)) !== 2) continue;
      if (deco === "bush") { if (r() < 0.8) paint([p[0] + dx, p[1] + dy], '"'); continue; }
      const side = dy === -2 ? 0 : dy === 2 ? 1 : dx === -2 ? 2 : 3;
      const mid = side < 2 ? Math.abs(dx) <= 1 : Math.abs(dy) <= 1;
      if (!(side === openSide && mid)) paint([p[0] + dx, p[1] + dy], "#");
    }
  }

  // camp groups
  let walk = 0;
  for (const row of T) for (const c of row) if (c !== "#" && c !== "o") walk++;
  const targetHalf = Math.round(Math.exp(Math.log(18) + r() * (Math.log(140) - Math.log(18))) / 2);
  const kindAt = new Map(), groups = [];
  let stored = 0;
  const campOk = (p, nearLine) => {
    if (!inMap(p) || get(p) !== "." || nearSpawn(p, 3) || lineDist(p) < (nearLine ? 0 : 2)) return false;
    if (altars.some((q) => cheb(p, q) <= 1 || cheb(mirror(p), q) <= 1)) return false;
    for (let dx = -2; dx <= 2; dx++) for (let dy = -2; dy <= 2; dy++) {
      const q = [p[0] + dx, p[1] + dy];
      if (inMap(q) && get(q) === "c") return false; // keep groups apart so they do not merge
    }
    return true;
  };
  const turned = (tpl) => {
    const rot = ri(0, 3), flip = r() < 0.5;
    const t = tpl.map(([k, dx, dy]) => { let x = flip ? -dx : dx, y = dy; for (let i = 0; i < rot; i++) [x, y] = [-y, x]; return [k, x, y]; });
    const mx = Math.min(...t.map((c) => c[1])), my = Math.min(...t.map((c) => c[2]));
    return t.map(([k, x, y]) => [k, x - mx, y - my]);
  };
  const tryPlace = (t, at, nearLine) => {
    const cells = t.map(([k, x, y]) => [k, [at[0] + x, at[1] + y]]);
    if (stored + cells.length > 240 || !cells.every(([, p]) => campOk(p, nearLine))) return false;
    for (const [k, p] of cells) { set(p, "c"); kindAt.set(idx(p), k); kindAt.set(idx(mirror(p)), k); }
    groups.push(cells.map(([, p]) => p)); stored += cells.length;
    return true;
  };
  let centrePiece = false;
  if (r() < 0.35) for (let tries = 0; tries < 60 && !centrePiece; tries++) {
    const t = turned(r() < 0.5 && BOSSES.length ? any(BOSSES) : any(CENTRE));
    const mx = Math.max(...t.map((c) => c[1])), my = Math.max(...t.map((c) => c[2]));
    const at = sym === 1 ? [Math.round((W - 1) / 2 - mx / 2) + ri(-3, 3), Math.floor((H - 1) / 2) - my]
             : [Math.floor((W - 1) / 2) - mx, Math.round((H - 1) / 2 - my / 2) + (sym === 0 ? ri(-Math.floor(H / 5), Math.floor(H / 5)) : ri(-1, 1))];
    centrePiece = tryPlace(t, at, true);
  }
  const fieldShare = cls === "dense" ? 0.25 : 0.03;
  // a drawn group gets many tries, so big groups are not lost to small ones that fit more easily
  for (let draws = 0; stored < targetHalf && draws < 300; draws++) {
    const tpl = r() < fieldShare && FIELDS.length ? any(FIELDS) : any(TEMPLATES);
    if (stored + tpl.length > targetHalf * 1.3 + 3) continue;
    for (let tries = 0; tries < 40 && !tryPlace(turned(tpl), rand(), false); tries++);
  }
  if (!groups.length) return { ok: false, why: "no groups" };

  // checks: walking (8 ways over ground, bush, spawn) from our spawn
  const D = new Int32Array(W * H).fill(-1);
  const queue = [spawn[0]]; D[idx(spawn[0])] = 0;
  for (let qi = 0; qi < queue.length; qi++) {
    const p = queue[qi];
    for (const [dx, dy] of N8) {
      const q = [p[0] + dx, p[1] + dy];
      if (!inMap(q) || D[idx(q)] >= 0 || !'."S'.includes(get(q))) continue;
      D[idx(q)] = D[idx(p)] + 1; queue.push(q);
    }
  }
  const touch = (p) => N8.some(([dx, dy]) => { const q = [p[0] + dx, p[1] + dy]; return inMap(q) && D[idx(q)] >= 0; });
  const enemyWalk = D[idx(spawnB[0])];
  const reachable = groups.filter((g) => g.some(touch)).length;
  let campsFull = 0;
  for (const row of T) for (const c of row) if (c === "c") campsFull++;
  const per1k = (1000 * campsFull) / walk;
  const why = enemyWalk < 0 ? "spawns cut apart"
    : enemyWalk > 2.2 * cheb(cA, cB) + 12 ? "maze"
    : !altars.every(touch) ? "altar cut off"
    : reachable < 0.9 * groups.length ? "groups cut off"
    : walk < 0.45 * W * H ? "too little ground"
    : groups.length < 3 || campsFull < 14 ? "too few camps"
    : cls === "dense" ? (per1k < 32 ? "not dense" : null) : (per1k >= 32 || per1k < 2 ? "not sparse" : null);
  return {
    ok: !why, why, W, H, sym, champs, spawnRect: [sx, sy, sx + sw - 1, sy + sw - 1], spawnStyle, T, kindAt, idx, mirror,
    stats: { wall: +shareOf("#").toFixed(3), hole: +shareOf("o").toFixed(3), bush: +shareOf('"').toFixed(3),
             altars: altars.reduce((n, p) => n + (idx(p) === idx(mirror(p)) ? 1 : 2), 0), campsFull,
             per1k: +per1k.toFixed(1), groupsHalf: groups.length, centrePiece, reach: `${reachable}/${groups.length}` },
  };
}

const KIND_OF = { "#": "wall", '"': "bush", o: "hole", A: "altar" };
const summary = [], why = {};
for (const cls of ["dense", "sparse"]) {
  for (let i = 0; i < PER_CLASS; i++) {
    const name = `gen_${cls}_${String(i).padStart(2, "0")}`;
    let made = null;
    for (let attempt = 0; attempt < 400 && !made; attempt++) {
      const m = build(cls, hash(name) + attempt * 7919);
      if (!m.ok) { why[m.why] = (why[m.why] || 0) + 1; continue; }
      const h = B.create(m.W, m.H, m.sym);
      try {
        B.setChamps(h, m.champs);
        B.setSpawn(h, ...m.spawnRect);
        for (let y = 0; y < m.H; y++) for (let x = 0; x < m.W; x++) {
          const p = [x, y];
          if (m.idx(p) > m.idx(m.mirror(p))) continue; // one tile of each mirror pair; the builder mirrors
          const c = m.T[y][x];
          if (KIND_OF[c]) B.add(h, KIND_OF[c], x, y);
          else if (c === "c") B.addCamp(h, m.kindAt.get(m.idx(p)), x, y);
        }
        const bytes = B.encode(h);
        const err = B.gameLoadError(bytes);
        if (err) { why[`loader ${err}`] = (why[`loader ${err}`] || 0) + 1; continue; }
        fs.writeFileSync(`${outDir}/${name}.map26a`, bytes);
        made = { name, W: m.W, H: m.H, sym: m.sym, champs: m.champs, spawn: m.spawnStyle, ...m.stats, attempt };
      } catch (e) {
        why[e.message.slice(0, 60)] = (why[e.message.slice(0, 60)] || 0) + 1;
      } finally { B.destroy(h); }
    }
    if (!made) console.log(`FAILED ${name}`);
    else summary.push(made);
  }
}
fs.writeFileSync(`${outDir}/gen_summary.json`, JSON.stringify(summary, null, 1));
for (const s of summary)
  console.log(`${s.name.padEnd(16)} ${String(s.W).padStart(3)}x${String(s.H).padEnd(3)} s${s.sym} c${s.champs} ${s.spawn.padEnd(6)} ` +
              `wall ${(s.wall * 100).toFixed(1).padStart(4)}% hole ${(s.hole * 100).toFixed(1).padStart(4)}% bush ${(s.bush * 100).toFixed(1).padStart(4)}% ` +
              `alt ${s.altars} camps ${String(s.campsFull).padStart(3)} per1k ${String(s.per1k).padStart(5)} groups/half ${String(s.groupsHalf).padStart(3)}` +
              `${s.centrePiece ? " centre" : ""} reach ${s.reach} (try ${s.attempt})`);
console.log("rejections:", JSON.stringify(why));
