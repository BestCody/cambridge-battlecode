// Measure the real maps a generator should resemble (the GA half only, so the
// holdout stays untouched): size, symmetry, champions, terrain shares and
// shapes, altars, spawn, camp groups (size, kinds, where they sit between the
// spawns), density. usage: node realstats.mjs <engine.wasm> <split.json> <out.json> [--ascii name]
import fs from "node:fs";
import { loadEngine } from "./maplib.mjs";

const [wasm, splitPath, outPath, flag, asciiName] = process.argv.slice(2);
const B = await loadEngine(wasm);
const split = JSON.parse(fs.readFileSync(splitPath, "utf8"));
const dir = process.env.MAPDIR ?? "C:/Users/louha/OneDrive/Desktop/cambridge-battlecode/maps/eval";

const comps = (rows, W, H, ch) => {
  const seen = new Set(), sizes = [];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    if (rows[y][x] !== ch || seen.has(y * W + x)) continue;
    let n = 0, minx = x, maxx = x, miny = y, maxy = y;
    const st = [[x, y]]; seen.add(y * W + x);
    while (st.length) {
      const [cx, cy] = st.pop(); n++;
      minx = Math.min(minx, cx); maxx = Math.max(maxx, cx); miny = Math.min(miny, cy); maxy = Math.max(maxy, cy);
      for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) {
        const nx = cx + dx, ny = cy + dy;
        if (nx < 0 || ny < 0 || nx >= W || ny >= H || rows[ny][nx] !== ch || seen.has(ny * W + nx)) continue;
        seen.add(ny * W + nx); st.push([nx, ny]);
      }
    }
    sizes.push({ n, span: Math.max(maxx - minx, maxy - miny) + 1 });
  }
  return sizes;
};

const out = [];
for (const name of split.ga) {
  const h = B.fromFile(`${dir}/${name}.map26a`);
  const i = B.info(h), t = B.terrain(h);
  const { W, H, rows } = t;
  if (flag === "--ascii" && name === asciiName) {
    const grid = rows.map((r) => r.split(""));
    for (const c of i.camps) grid[c.y][c.x] = "wegdmG"[["wolf", "elk", "gem_cluster", "gold_cluster", "monolith", "golem"].indexOf(c.kind)];
    const s = i.spawnA;
    for (let y = s.minY; y <= s.maxY; y++) for (let x = s.minX; x <= s.maxX; x++) grid[y][x] = "S";
    console.log(grid.map((r) => r.join("")).join("\n"));
  }
  const count = (ch) => rows.reduce((a, r) => a + [...r].filter((c) => c === ch).length, 0);
  const tiles = W * H, walls = count("#"), holes = count("o"), bushes = count('"'), altars = count("A");
  const walk = tiles - walls - holes;
  // spawn centres (team B mirrored)
  const s = i.spawnA, sa = [(s.minX + s.maxX) / 2, (s.minY + s.maxY) / 2];
  const mir = ([x, y]) => (i.symmetry === 0 ? [W - 1 - x, y] : i.symmetry === 1 ? [x, H - 1 - y] : [W - 1 - x, H - 1 - y]);
  const sb = mir(sa);
  const d = (p, q) => Math.hypot(p[0] - q[0], p[1] - q[1]);
  // groups: camps per group, kinds, position share between the spawns (0 = own spawn, 0.5 = middle)
  const campsOf = new Map();
  for (const c of i.camps) campsOf.set(`${c.x},${c.y}`, c);
  const groups = [];
  {
    // regroup camps by adjacency (8-way), as the engine does, on the full map
    const all = [];
    for (const c of i.camps) { all.push(c); const m = mir([c.x, c.y]); if (m[0] !== c.x || m[1] !== c.y) all.push({ ...c, x: m[0], y: m[1] }); }
    const key = (c) => `${c.x},${c.y}`, idx = new Map(all.map((c, k) => [key(c), k])), seen = new Set();
    for (let k = 0; k < all.length; k++) {
      if (seen.has(k)) continue;
      const g = [], st = [k]; seen.add(k);
      while (st.length) {
        const j = st.pop(); g.push(all[j]);
        for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) {
          const n = idx.get(`${all[j].x + dx},${all[j].y + dy}`);
          if (n !== undefined && !seen.has(n)) { seen.add(n); st.push(n); }
        }
      }
      const cx = g.reduce((a, c) => a + c.x, 0) / g.length, cy = g.reduce((a, c) => a + c.y, 0) / g.length;
      const share = d([cx, cy], sa) / (d([cx, cy], sa) + d([cx, cy], sb));
      const kinds = {};
      for (const c of g) kinds[c.kind] = (kinds[c.kind] || 0) + 1;
      const onBush = g.filter((c) => rows[c.y][c.x] === '"').length;
      // shape for templates: [kind, dx, dy] from the group's top-left
      const mx = Math.min(...g.map((c) => c.x)), my = Math.min(...g.map((c) => c.y));
      const cells = g.map((c) => [c.kind, c.x - mx, c.y - my]);
      groups.push({ n: g.length, kinds, share: +share.toFixed(3), onBush, cells });
    }
  }
  const wallC = comps(rows, W, H, "#"), holeC = comps(rows, W, H, "o"), bushC = comps(rows, W, H, '"');
  const big = (cs) => cs.filter((c) => c.span >= 8).reduce((a, c) => a + c.n, 0);
  const altarPos = [];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (rows[y][x] === "A")
    altarPos.push(+(d([x, y], sa) / (d([x, y], sa) + d([x, y], sb))).toFixed(3));
  out.push({
    map: name, W, H, sym: i.symmetry, champs: i.champs,
    spawn: { w: s.maxX - s.minX + 1, h: s.maxY - s.minY + 1, cx: +(sa[0] / (W - 1)).toFixed(2), cy: +(sa[1] / (H - 1)).toFixed(2) },
    spawnGap: +(d(sa, sb) / Math.hypot(W, H)).toFixed(3),
    wall: +(walls / tiles).toFixed(4), hole: +(holes / tiles).toFixed(4), bush: +(bushes / tiles).toFixed(4), altars,
    altarShare: altarPos, wallComps: wallC.length, wallLongShare: walls ? +(big(wallC) / walls).toFixed(2) : 0,
    holeComps: holeC.length, holeLongShare: holes ? +(big(holeC) / holes).toFixed(2) : 0,
    bushComps: bushC.length, bushMean: bushC.length ? +(bushes / bushC.length).toFixed(1) : 0,
    camps: groups.reduce((a, g) => a + g.n, 0), perK: +(1000 * groups.reduce((a, g) => a + g.n, 0) / walk).toFixed(1),
    groups,
  });
  B.destroy(h);
}
fs.writeFileSync(outPath, JSON.stringify(out));
if (flag !== "--ascii") {
  for (const m of out)
    console.log(`${m.map.padEnd(22)} ${m.W}x${m.H} s${m.sym} c${m.champs} sp ${m.spawn.w}x${m.spawn.h}@${m.spawn.cx},${m.spawn.cy} gap ${m.spawnGap} ` +
                `wall ${(m.wall * 100).toFixed(1)}% (${m.wallComps}, long ${m.wallLongShare}) hole ${(m.hole * 100).toFixed(1)}% (${m.holeComps}, long ${m.holeLongShare}) ` +
                `bush ${(m.bush * 100).toFixed(1)}% (${m.bushComps}x${m.bushMean}) alt ${m.altars} [${m.altarShare.join(" ")}] ` +
                `camps ${m.camps} per1k ${m.perK} groups ${m.groups.length}`);
}
