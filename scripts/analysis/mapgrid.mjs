// Print a replay's static map: # wall, ~ bush, o hole, A altar, camp letters, spawn digits.
import { loadEngine, readReplayBytes, Replay } from "./engine.mjs";
const eng = await loadEngine();
const r = new Replay(eng, readReplayBytes(process.argv[2]));
// Map masks need an owned champion's perspective: step until one exists.
let found = false;
for (let i = 0; i < 200 && !found && !r.finished(); i++) {
  const h = r.next();
  for (const hd of [h.handle, h.champion, h.actor].filter(v => v !== undefined)) {
    for (const t of [0, 1]) { r.setPerspective(t, hd); if (r.identity(hd)) { found = true; break; } }
    if (found) break;
  }
}
if (!found) console.log("no champion handle found; header keys:", Object.keys(r.head).join(" "));
const names = ["Game_API_Map_GetWallBitboard","Game_API_Map_GetBushBitboard","Game_API_Map_GetHoleBitboard","Game_API_Map_GetAltarBitboard"];
console.log("mask exports:", names.join(" "));
const W = r.W, H = r.H;
const grid = Array.from({ length: H }, () => Array(W).fill("."));
const mark = (name, ch) => {
  try { const b = r.mapBoard(name); for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (b[y * W + x]) grid[y][x] = ch; }
  catch (e) { console.log("no", name, e.message); }
};
for (const n of names) mark(n, /Wall/i.test(n) ? "#" : /Bush/i.test(n) ? "~" : /Hole/i.test(n) ? "o" : /Altar/i.test(n) ? "A" : "?");
const types = { 0: "w", 1: "e", 2: "g", 3: "$", 4: "M", 5: "G" };
for (const g of r.campLayout()) for (const c of g.camps) grid[c.y][c.x] = types[c.type] ?? "c";
for (const t of [0, 1]) { const s = r.spawnRegion(t); for (let y = s.y0; y < s.y1; y++) for (let x = s.x0; x < s.x1; x++) grid[y][x] = String(t); }
console.log("    " + Array.from({ length: W }, (_, x) => x % 10).join(""));
for (let y = 0; y < H; y++) console.log(String(y).padStart(3) + " " + grid[y].join(""));
