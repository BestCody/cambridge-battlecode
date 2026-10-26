// Record a window of a replay step by step (every move, attack and ability),
// for drawing fights. Re-simulates the replay with the visualiser's engine
// module. Output JSON: the map (walls, bushes, altars as rows of '#', '"', 'A'
// and '.'), and for each step in rounds [from, to]: round, step kind, actor,
// from/to tiles, ability slot/type, and every champion's position, health,
// alive and stunner; per round, the camps and which enemy champions each
// champion could see.
//
// usage: node watch_fight.mjs <engine.wasm> <match zip> <from round> <to round> <out.json>
import fs from "node:fs";
import zlib from "node:zlib";

const [wasmPath, replayPath, fromArg, toArg, outPath] = process.argv.slice(2);
const R0 = +fromArg, R1 = +toArg;

function readReplayBytes(path) {
  const buf = fs.readFileSync(path);
  if (buf[0] !== 0x50 || buf[1] !== 0x4b) return buf;
  const eocd = buf.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  const cdOffset = buf.readUInt32LE(eocd + 16), cdCount = buf.readUInt16LE(eocd + 10);
  let p = cdOffset;
  for (let i = 0; i < cdCount; i++) {
    const method = buf.readUInt16LE(p + 10), csize = buf.readUInt32LE(p + 20);
    const nlen = buf.readUInt16LE(p + 28), elen = buf.readUInt16LE(p + 30), clen = buf.readUInt16LE(p + 32);
    const local = buf.readUInt32LE(p + 42), name = buf.toString("utf8", p + 46, p + 46 + nlen);
    if (name === "replay.repl26a") {
      const lnlen = buf.readUInt16LE(local + 26), lelen = buf.readUInt16LE(local + 28);
      const data = buf.subarray(local + 30 + lnlen + lelen, local + 30 + lnlen + lelen + csize);
      return method === 8 ? zlib.inflateRawSync(data) : data;
    }
    p += 46 + nlen + elen + clen;
  }
  throw new Error("replay.repl26a not found in zip");
}

let memory;
const view = () => new DataView(memory.buffer);
const decoder = new TextDecoder();
const cstr = (ptr) => {
  const b = new Uint8Array(memory.buffer);
  let e = ptr;
  while (e < b.length && b[e] !== 0) e++;
  return decoder.decode(b.subarray(ptr, e));
};
const imports = {
  wasi_snapshot_preview1: {
    random_get: (ptr, len) => { new Uint8Array(memory.buffer, ptr, len).fill(7); return 0; },
    clock_time_get: (id, prec, out) => { view().setBigUint64(out, 0n, true); return 0; },
    environ_sizes_get: (a, b) => { view().setUint32(a, 0, true); view().setUint32(b, 0, true); return 0; },
    environ_get: () => 0,
    args_sizes_get: (a, b) => { view().setUint32(a, 0, true); view().setUint32(b, 0, true); return 0; },
    args_get: () => 0,
    fd_close: (fd) => (fd > 2 ? 8 : 0),
    fd_fdstat_get: (fd, out) => {
      if (fd > 2) return 8;
      const v = view(); v.setUint8(out, 2); v.setUint16(out + 2, 0, true);
      v.setBigUint64(out + 8, 0n, true); v.setBigUint64(out + 16, 0n, true); return 0;
    },
    fd_prestat_get: () => 8,
    fd_prestat_dir_name: () => 8,
    fd_read: (fd, iovs, n, out) => { if (fd !== 0) return 8; view().setUint32(out, 0, true); return 0; },
    fd_seek: (fd) => (fd > 2 ? 8 : 70),
    fd_write: (fd, iovs, n, out) => {
      const v = view(); let total = 0;
      for (let i = 0; i < n; i++) total += v.getUint32(iovs + i * 8 + 4, true);
      v.setUint32(out, total, true); return 0;
    },
    proc_exit: (code) => { throw new Error(`wasm exit ${code}`); },
  },
  deep_wasm: { on_assert_fail_impl: (a, f, l, m) => { console.error(`assert ${cstr(f)}:${l} ${cstr(a)}`); return 1; } },
};

const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), imports);
const x = instance.exports;
memory = x.memory;
x._initialize();
const outBuf = x.Engine_Malloc(32), args = x.Engine_Malloc(128);
const check = (code, what) => {
  if (code !== 0) throw new Error(`${what} failed (${code}): ${cstr(x.GetLastErrorMsg())}`);
};

const bytes = readReplayBytes(replayPath);
const rp = x.Engine_Malloc(bytes.length);
new Uint8Array(memory.buffer, rp, bytes.length).set(bytes);
check(x.Game_FromReplayFileBytes(rp, bytes.length, outBuf), "Game_FromReplayFileBytes");
const game = view().getBigUint64(outBuf, true);
x.Engine_Free(rp);
check(x.Game_Visualiser_Timeline_GetMain(game, outBuf), "Timeline_GetMain");
const timeline = view().getBigUint64(outBuf, true);
check(x.Game_Visualiser_Timeline_Info(game, timeline, args, args + 8, args + 16), "Timeline_Info");
const size = view().getUint32(args, true);
check(x.Game_Visualiser_Timeline_Seek(game, timeline, 0, 0), "Seek");
if (x.Game_Visualiser_TimelineSnapshotSize() !== 40152) throw new Error("unexpected snapshot size");
const snap = x.Engine_Malloc(40152);

// Bitboards: 8x8 chunks of u64, as the visualiser's test().
check(x.Game_GetBitboardMetadata(game, outBuf), "Metadata");
const W = view().getInt32(outBuf, true), H = view().getInt32(outBuf + 4, true), stride = view().getUint32(outBuf + 16, true);
const test = (ptr, px, py) => {
  const a = (py >> 3) * stride + (px >> 3), o = ((py & 7) << 3) + (px & 7);
  return (view().getBigUint64(ptr + a * 8, true) >> BigInt(o)) & 1n;
};
const board = (fn) => { check(fn(game, outBuf), "board"); return view().getUint32(outBuf, true); };
const walls = board(x.Game_GetWallBuffer), bushes = board(x.Game_GetBushBuffer), altars = board(x.Game_GetAltarBuffer);
const rows = [];
for (let py = 0; py < H; py++) {
  let s = "";
  for (let px = 0; px < W; px++) s += test(walls, px, py) ? "#" : test(bushes, px, py) ? '"' : test(altars, px, py) ? "A" : ".";
  rows.push(s);
}

const persp = args + 48;
// What one champion sees (vision is per champion).
function visibleTo(team, champion, px, py) {
  const v = view();
  v.setUint32(persp, team, true);
  v.setUint32(persp + 4, champion, true);
  check(x.Game_Visualiser_GetVisionBuffer(game, persp, outBuf), "Vision");
  return !!test(view().getUint32(outBuf, true), px, py);
}

function readState() {
  check(x.Game_Visualiser_ReadTimelineState(game, snap), "ReadTimelineState");
  const v = view(), u = (o) => v.getUint32(snap + o, true), i = (o) => v.getInt32(snap + o, true);
  const f = (o) => v.getFloat32(snap + o, true);
  const slot = u(108);
  const head = { round: u(8), outcome: u(28), awaiting: !!u(20), input: u(56),
                 step: { kind: u(68), ak: u(72), a: u(76), fx: i(80), fy: i(84), tx: i(88), ty: i(92),
                         slot: slot === 0xffffffff ? -1 : slot, at: slot === 0xffffffff ? -1 : u(112) } };
  const champions = [];
  for (let k = 0, n = u(172); k < n; k++) {
    const b = 196 + k * 252;
    champions.push({ id: u(b + 4), team: u(b + 8), type: u(b + 12), x: i(b + 16), y: i(b + 20),
                     hp: Math.round(f(b + 24)), maxHp: Math.round(f(b + 28)), alive: !!u(b + 32),
                     gold: u(b + 36), gems: u(b + 40), kills: u(b + 44),
                     stun: u(b + 248) ? [u(b + 240), u(b + 244)] : null });
  }
  const camps = [];
  for (let k = 0, n = u(176); k < n; k++) {
    const b = 4228 + k * 28;
    const hp = f(b + 24);
    if (hp > 0) camps.push([u(b + 4), u(b + 12), i(b + 16), i(b + 20), Math.round(hp)]);
  }
  return { head, champions, camps };
}

const steps = [], perRound = {};
let state = readState(), lastRound = -1;
for (;;) {
  const r = state.head.round;
  if (r > R1 || state.head.outcome) break;
  if (r >= R0) {
    if (r !== lastRound) {
      // Round start: camps, and which enemy champions each champion sees.
      const seen = {};
      for (const me of state.champions) {
        if (!me.alive || me.x < 0) continue;
        seen[me.id] = state.champions.filter((c) => c.alive && c.x >= 0 && c.team !== me.team && visibleTo(me.team, me.id, c.x, c.y)).map((c) => c.id);
      }
      perRound[r] = { camps: state.camps, seen, champions: state.champions.map((c) => ({ id: c.id, team: c.team, type: c.type, gold: c.gold, gems: c.gems, kills: c.kills, maxHp: c.maxHp })) };
    }
    const s = state.head.step;
    steps.push({ r, ...s, c: state.champions.map((c) => [c.id, c.x, c.y, c.hp, c.alive ? 1 : 0, c.stun ? c.stun[1] : -1]) });
  }
  lastRound = r;
  if (state.head.awaiting && state.head.input >= size) break;
  check(x.Game_Visualiser_Timeline_ReplayNext(game, 1), "ReplayNext");
  state = readState();
}
fs.writeFileSync(outPath, JSON.stringify({ width: W, height: H, map: rows, rounds: perRound, steps }));
console.log(`steps=${steps.length} rounds=${Object.keys(perRound).length}`);
