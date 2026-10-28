// Run the engine's own DistanceField_Compute (the routine behind the SDK's distance_field::compute)
// on a map from an extracted game, so features.py's BFS can be checked against what a bot computes.
//
// usage: node sdk_distance.mjs <game.jsonl.gz> <out.json>
// Output: {W, H, fields: {own, enemy, altar, group0, group1, ...}} as row-major arrays (65535 = unreachable).
import fs from "node:fs";
import zlib from "node:zlib";
import { loadEngine } from "./engine.mjs";

const [gamePath, outPath] = process.argv.slice(2);
const header = JSON.parse(zlib.gunzipSync(fs.readFileSync(gamePath)).toString("utf8").split("\n", 1)[0]).header;
const eng = await loadEngine();
const { x } = eng, v = () => eng.view();
const W = header.W, H = header.H;
const meta = x.Engine_Malloc(64);
const check = (c, what) => { if (c !== 0) throw new Error(`${what} failed (${c}): ${eng.cstr(x.GetLastErrorMsg())}`); };
check(x.Bitboard_Metadata_Init(W, H, meta), "Bitboard_Metadata_Init");
const size = (fn) => { const o = x.Engine_Malloc(8); check(x[fn](meta, o), fn); return v().getUint32(o, true); };
const boardBytes = size("Bitboard_GetRawBufferSize"), fieldBytes = size("DistanceField_GetRawBufferSize");
const linBits = size("Bitboard_GetLinearBufferSize"), linField = size("DistanceField_GetLinearBufferSize");
const alloc = (n) => { const p = x.Engine_Malloc(n + 64); return (p + 15) & ~15; };

function board(pred) { // row-major predicate -> engine-chunked bitboard (linear form is bit-packed, LSB first)
  const lin = x.Engine_Malloc(linBits), b = alloc(boardBytes);
  const bytes = new Uint8Array(eng.memory.buffer, lin, linBits);
  bytes.fill(0);
  for (let y = 0; y < H; y++) for (let xx = 0; xx < W; xx++) if (pred(xx, y)) { const i = y * W + xx; bytes[i >> 3] |= 1 << (i & 7); }
  check(x.Bitboard_ImportLinearBuffer(meta, b, lin, linBits), "Bitboard_ImportLinearBuffer");
  return b;
}
const at = (rows, xx, y) => rows[y][xx] === "1";
const inRect = (r, xx, y) => xx >= r.x0 && xx < r.x1 && y >= r.y0 && y < r.y1;
const own = header.spawns[header.team], enemy = header.spawns[1 - header.team];
const blocked = (xx, y) => at(header.walls, xx, y) || at(header.holes, xx, y) || at(header.altars, xx, y) || inRect(enemy, xx, y);
const near = (pred, xx, y) => { for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) { const a = xx + dx, b = y + dy; if (a >= 0 && b >= 0 && a < W && b < H && pred(a, b)) return true; } return false; };

const walls = board(blocked), visited = alloc(boardBytes), dist = alloc(fieldBytes), lin = alloc(linField);
function compute(rootPred) {
  const roots = board(rootPred);
  check(x.DistanceField_Compute(meta, walls, roots, visited, dist, 0xffff), "DistanceField_Compute");
  check(x.DistanceField_ExportLinearBuffer(meta, dist, lin, linField), "DistanceField_ExportLinearBuffer");
  return Array.from(new Uint16Array(eng.memory.buffer, lin, W * H));
}
const fields = {
  own: compute((xx, y) => inRect(own, xx, y)),
  enemy: compute((xx, y) => !blocked(xx, y) && near((a, b) => inRect(enemy, a, b), xx, y)),
  altar: compute((xx, y) => !blocked(xx, y) && near((a, b) => at(header.altars, a, b), xx, y)),
};
header.groups.forEach((g, i) => {
  const tiles = new Set(g.camps.map((c) => c.y * W + c.x));
  fields[`group${i}`] = compute((xx, y) => tiles.has(y * W + xx));
});
fs.writeFileSync(outPath, JSON.stringify({ W, H, fields }));
console.log(`fields=${Object.keys(fields).length}`);
