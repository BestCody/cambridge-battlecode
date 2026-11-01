// Edge-case map generator: 15 themes x {dense, sparse}. Only the canonical
// half is placed (the engine mirrors it, as real map files store it); the
// checks run on the full mirrored map.
// usage: node genmaps.mjs <engine.wasm> <out dir>
import fs from "node:fs";
import { loadEngine, CAMP } from "./maplib.mjs";

const [wasm, outDir] = process.argv.slice(2);
const B = await loadEngine(wasm);
fs.mkdirSync(outDir, { recursive: true });

const K = CAMP;
const THEMES = [
  { name: "all_monolith", kinds: [[K.monolith, 1]], group: [1, 3] },
  { name: "all_golem", kinds: [[K.golem, 1]], group: [1, 4] },
  { name: "gold_only", kinds: [[K.gold_cluster, 1]], group: [2, 8] },
  { name: "gems_only", kinds: [[K.gem_cluster, 1]], group: [2, 8] },
  { name: "wolves_only", kinds: [[K.wolf, 1]], group: [1, 4] },
  { name: "elk_only", kinds: [[K.elk, 1]], group: [1, 3] },
  { name: "one_centre_altar", altars: 0, centreAltar: true, odd: true },
  { name: "many_altars", altars: 12 },
  { name: "bush_jungle", bush: 0.45 },
  { name: "hole_islands", islands: true },
  { name: "walled_pockets", pockets: true },
  { name: "lanes", lanes: true, walls: 0 },
  { name: "mega_group", mega: true },
  { name: "camps_hug_spawn", hug: true },
  { name: "size_tiny", size: [29, 29], sym: 2 },
  { name: "size_huge", size: [100, 100], sym: 2 },
];
// The ordinary mix where a theme does not set the kinds (the pool's shares).
const MIX = [[K.gem_cluster, 34], [K.gold_cluster, 30], [K.wolf, 15], [K.elk, 9], [K.golem, 1], [K.monolith, 1]];
const DENSITY = { dense: 0.08, sparse: 0.008 }; // camps per walkable tile, full map

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

function build(theme, density, seed) {
  const r = rng(seed);
  // size, symmetry, champions
  let [W, H] = theme.size ?? [30 + Math.floor(r() * 50), 30 + Math.floor(r() * 45)];
  if (theme.odd) { W |= 1; H |= 1; }
  const sym = theme.sym ?? pickW(r, [[2, 63], [0, 23], [1, 14]]);
  const champs = pickW(r, [[5, 60], [4, 23], [3, 17]]);
  const mirror = ([x, y]) => (sym === 0 ? [W - 1 - x, y] : sym === 1 ? [x, H - 1 - y] : [W - 1 - x, H - 1 - y]);
  // canonical half: strictly before the mirror line
  const inHalf = ([x, y]) => (sym === 1 ? y < Math.floor(H / 2) : x < Math.floor(W / 2));
  const lineDist = ([x, y]) => (sym === 1 ? Math.floor((H - 1) / 2) - y : Math.floor((W - 1) / 2) - x);
  const T = Array.from({ length: H }, () => new Array(W).fill("."));
  const set = (p, ch) => { T[p[1]][p[0]] = ch; const m = mirror(p); T[m[1]][m[0]] = ch; };
  const placed = { wall: [], bush: [], hole: [], altar: [], camps: [] };
  const free = (p) => p[0] >= 0 && p[1] >= 0 && p[0] < W && p[1] < H && T[p[1]][p[0]] === ".";
  // spawn: 4x4 at the half's outer edge, centred
  const sp = sym === 1 ? [Math.floor(W / 2) - 2, 2] : [2, Math.floor(H / 2) - 2];
  const spawn = [];
  for (let dx = 0; dx < 4; dx++) for (let dy = 0; dy < 4; dy++) spawn.push([sp[0] + dx, sp[1] + dy]);
  for (const p of spawn) set(p, "S");
  const nearSpawn = (p, d) => spawn.some((s) => cheb(s, p) <= d) || spawn.some((s) => cheb(mirror(s), p) <= d);
  const halfTiles = [];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (inHalf([x, y])) halfTiles.push([x, y]);
  const randHalf = () => halfTiles[Math.floor(r() * halfTiles.length)];
  const put = (kind, p) => { set(p, { wall: "#", bush: '"', hole: "o", altar: "A" }[kind]); placed[kind].push(p); };

  // terrain: walls as short segments
  const wallShare = theme.walls ?? 0.05 + r() * 0.07;
  let walls = 0;
  while (walls < wallShare * halfTiles.length && !theme.lanes) {
    let p = randHalf();
    const dir = r() < 0.5 ? [1, 0] : [0, 1], len = 2 + Math.floor(r() * 6);
    for (let k = 0; k < len; k++, p = [p[0] + dir[0], p[1] + dir[1]])
      if (free(p) && inHalf(p) && !nearSpawn(p, 2) && lineDist(p) >= 1) { put("wall", p); walls++; }
    if (walls === 0 && r() < 0.01) break;
  }
  if (theme.lanes) {
    // two long walls along the attack axis, open near each spawn's side
    const long = sym === 1 ? H : W, across = sym === 1 ? W : H;
    for (const frac of [1 / 3, 2 / 3]) {
      const c = Math.floor(across * frac);
      for (let a = 8; a < Math.floor(long / 2); a++) {
        const p = sym === 1 ? [c, a] : [a, c];
        if (free(p) && !nearSpawn(p, 2)) put("wall", p);
      }
    }
  }
  // bushes
  const bushShare = theme.bush ?? 0.02 + r() * 0.06;
  for (let n = 0; n < bushShare * halfTiles.length * 1.3; n++) {
    const p = randHalf();
    if (free(p) && !nearSpawn(p, 1) && lineDist(p) >= 1) put("bush", p);
  }
  // altars
  if (theme.centreAltar) {
    const c = [Math.floor((W - 1) / 2), Math.floor((H - 1) / 2)];
    T[c[1]][c[0]] = "A"; placed.altar.push(c);
  }
  const nAlt = theme.altars ?? 1 + Math.floor(r() * 2);
  for (let n = 0, tries = 0; n < nAlt && tries < 5000; tries++) {
    const p = randHalf();
    if (free(p) && !nearSpawn(p, 3) && lineDist(p) >= 2) { put("altar", p); n++; }
  }

  // camp groups
  const walkable = () => T.flat().filter((c) => c !== "#" && c !== "o").length;
  // The map format holds at most 250 camps (the stored half).
  const targetHalf = Math.min(240, Math.max(4, Math.round((DENSITY[density] * walkable()) / 2)));
  const groups = []; // each: list of tiles (canonical half)
  const grow = (seedTile, size) => {
    const g = [seedTile];
    const seen = new Set([seedTile.join()]);
    for (let i = 0; g.length < size && i < 400; i++) {
      const base = g[Math.floor(r() * g.length)];
      const q = [base[0] + [1, -1, 0, 0][i % 4], base[1] + [0, 0, 1, -1][i % 4]];
      if (seen.has(q.join()) || !free(q) || !inHalf(q)) continue;
      seen.add(q.join());
      if (lineDist(q) < (theme.mega ? 0 : 2)) continue;
      if (groups.some((o) => o.some((t) => cheb(t, q) <= 2))) continue;
      if (nearSpawn(q, theme.hug ? 1 : 3)) continue;
      g.push(q);
    }
    return g;
  };
  let campCount = 0;
  if (theme.mega) {
    // one giant group touching the mirror line: it merges with its mirror
    const c = sym === 1 ? [Math.floor(W / 2), Math.floor(H / 2) - 1] : [Math.floor(W / 2) - 1, Math.floor(H / 2)];
    const size = Math.max(10, Math.round(targetHalf * 0.7));
    const g = grow(c, size);
    groups.push(g); campCount += g.length;
  }
  for (let tries = 0; campCount < targetHalf && tries < 20000; tries++) {
    let p = randHalf();
    if (theme.hug) {
      const s = spawn[Math.floor(r() * spawn.length)];
      const span = density === "dense" ? 12 : 7;
      p = [s[0] + Math.floor(r() * (2 * span + 1)) - span, s[1] + Math.floor(r() * (2 * span + 1)) - span];
      if (!(p[0] >= 0 && p[1] >= 0 && p[0] < W && p[1] < H) || !inHalf(p)) continue;
    }
    if (!free(p) || nearSpawn(p, theme.hug ? 1 : 3) || lineDist(p) < 2) continue;
    if (groups.some((o) => o.some((t) => cheb(t, p) <= 2))) continue;
    const [lo, hi] = theme.group ?? [1, 6];
    const g = grow(p, lo + Math.floor(r() * (hi - lo + 1)));
    groups.push(g); campCount += g.length;
    for (const t of g) T[t[1]][t[0]] = "c";
  }
  for (const g of groups) for (const t of g) set(t, "c");
  // islands: a hole moat (1-2 wide, dash crosses) around a share of groups;
  // pockets: a sealed wall ring (crystal only) around a share of groups
  const sealed = [];
  if (theme.islands || theme.pockets) {
    for (const g of groups) {
      if (r() > 0.4) continue;
      const ring = new Set();
      for (const t of g) for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) {
        const q = [t[0] + dx, t[1] + dy];
        if (q[0] >= 0 && q[1] >= 0 && q[0] < W && q[1] < H && T[q[1]][q[0]] !== "c") ring.add(q.join());
      }
      const ringT = [...ring].map((s) => s.split(",").map(Number));
      if (ringT.some((q) => !inHalf(q) || nearSpawn(q, 1) || T[q[1]][q[0]] === "A")) continue;
      for (const q of ringT) { if (T[q[1]][q[0]] === '"') T[q[1]][q[0]] = "."; put(theme.islands ? "hole" : "wall", q); }
      sealed.push(g);
    }
  }
  // camp kinds per group
  const kinds = theme.kinds ?? MIX;
  for (const g of groups) {
    const k = pickW(r, kinds);
    for (const t of g) placed.camps.push([theme.kinds ? pickW(r, kinds) : k, t]);
  }

  // check: walking from spawn (8 ways over ground and bush)
  const reach = Array.from({ length: H }, () => new Array(W).fill(false));
  const q = [spawn[0]]; reach[spawn[0][1]][spawn[0][0]] = true;
  while (q.length) {
    const [x, y] = q.pop();
    for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) {
      const nx = x + dx, ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= W || ny >= H || reach[ny][nx]) continue;
      if (!".\"S".includes(T[ny][nx])) continue;
      reach[ny][nx] = true; q.push([nx, ny]);
    }
  }
  const touch = (p) => [-1, 0, 1].some((dx) => [-1, 0, 1].some((dy) => {
    const nx = p[0] + dx, ny = p[1] + dy;
    return nx >= 0 && ny >= 0 && nx < W && ny < H && reach[ny][nx];
  }));
  const reachableGroups = groups.filter((g) => g.some(touch)).length;
  const reachableAltars = placed.altar.filter(touch).length;
  const enemySpawnReached = touch(mirror(spawn[0]));
  const needAll = !(theme.islands || theme.pockets);
  const ok = enemySpawnReached && reachableAltars >= 1 && groups.length > 0 &&
             (needAll ? reachableGroups === groups.length : reachableGroups >= Math.ceil(groups.length * 0.5));
  return { ok, W, H, sym, champs, spawn, placed, groups, sealed: sealed.length, reachableGroups, campCount,
           walkable: walkable(), T };
}

const summary = [];
for (const theme of THEMES) {
  for (const density of ["dense", "sparse"]) {
    const name = `edge_${theme.name}_${density}`;
    let made = null;
    for (let attempt = 0; attempt < 60 && !made; attempt++) {
      const m = build(theme, density, hash(name) + attempt * 7919);
      if (!m.ok) continue;
      const h = B.create(m.W, m.H, m.sym);
      try {
        B.setChamps(h, m.champs);
        const xs = m.spawn.map((p) => p[0]), ys = m.spawn.map((p) => p[1]);
        B.setSpawn(h, Math.min(...xs), Math.min(...ys), Math.max(...xs), Math.max(...ys));
        for (const kind of ["wall", "bush", "hole", "altar"]) for (const p of m.placed[kind]) B.add(h, kind, p[0], p[1]);
        for (const [k, p] of m.placed.camps) B.addCamp(h, k, p[0], p[1]);
        const bytes = B.encode(h);
        const err = B.gameLoadError(bytes);
        if (err) { console.log(`  ${name} attempt ${attempt}: loader ${err}`); continue; }
        fs.writeFileSync(`${outDir}/${name}.map26a`, bytes);
        const info = B.info(h);
        made = { name, W: m.W, H: m.H, sym: m.sym, champs: m.champs, campsHalf: info.camps.length,
                 groupsHalf: info.groups.length, perK: +(1000 * 2 * info.camps.length / m.walkable).toFixed(1),
                 altars: m.placed.altar.length, walled: m.sealed, reachable: `${m.reachableGroups}/${m.groups.length}`,
                 maxGroup: Math.max(...info.groups.map((g) => g.camps)) };
      } catch (e) {
        console.log(`  ${name} attempt ${attempt}: ${e.message}`);
      } finally { B.destroy(h); }
    }
    if (!made) console.log(`FAILED ${name}`);
    else summary.push(made);
  }
}
fs.writeFileSync(`${outDir}/summary.json`, JSON.stringify(summary, null, 1));
for (const s of summary)
  console.log(`${s.name.padEnd(34)} ${String(s.W).padStart(3)}x${String(s.H).padEnd(3)} sym ${s.sym} champs ${s.champs} ` +
              `camps/half ${String(s.campsHalf).padStart(3)} groups ${String(s.groupsHalf).padStart(3)} ` +
              `per1000 ${String(s.perK).padStart(5)} biggest ${String(s.maxGroup).padStart(3)} altars ${s.altars} ` +
              `reachable ${s.reachable}${s.walled ? ` sealed ${s.walled}` : ""}`);
