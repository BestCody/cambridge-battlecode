// Thin wrapper around the visualiser's engine 2.0.0 wasm module: load a replay, step through its
// timeline, and query the game through the same Game_API_* host functions the bot SDK calls, from a
// chosen champion's perspective. Because the engine itself enforces vision for these queries,
// anything read here is exactly what that champion's bot could have read with the SDK.
//
// Signatures were read from the wasm type section; each Game_API_* function takes
// (game:i64, perspective*:i32, ...the SDK import's arguments...). Records follow sdk/cpp/imports.h.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import zlib from "node:zlib";

export function findEngineWasm() {
  if (process.env.CAMBC_ENGINE_WASM) return process.env.CAMBC_ENGINE_WASM;
  const root = path.join(os.homedir(), "AppData", "Local", "cambc", "assets");
  for (const hash of fs.existsSync(root) ? fs.readdirSync(root) : []) {
    const dir = path.join(root, hash, "visualiser", "assets");
    if (!fs.existsSync(dir)) continue;
    const f = fs.readdirSync(dir).find((n) => /^v2_0_0-.*\.wasm$/.test(n));
    if (f) return path.join(dir, f);
  }
  throw new Error("engine 2.0.0 wasm not found; set CAMBC_ENGINE_WASM");
}

export function readReplayBytes(file) {
  const buf = fs.readFileSync(file);
  if (buf[0] !== 0x50 || buf[1] !== 0x4b) return buf; // not a zip
  const eocd = buf.lastIndexOf(Buffer.from([0x50, 0x4b, 0x05, 0x06]));
  const cdOffset = buf.readUInt32LE(eocd + 16), cdCount = buf.readUInt16LE(eocd + 10);
  let p = cdOffset;
  for (let i = 0; i < cdCount; i++) {
    const method = buf.readUInt16LE(p + 10), csize = buf.readUInt32LE(p + 20);
    const nlen = buf.readUInt16LE(p + 28), elen = buf.readUInt16LE(p + 30), clen = buf.readUInt16LE(p + 32);
    const local = buf.readUInt32LE(p + 42), name = buf.toString("utf8", p + 46, p + 46 + nlen);
    if (name.endsWith(".repl26a")) {
      const lnlen = buf.readUInt16LE(local + 26), lelen = buf.readUInt16LE(local + 28);
      const data = buf.subarray(local + 30 + lnlen + lelen, local + 30 + lnlen + lelen + csize);
      return method === 8 ? zlib.inflateRawSync(data) : data;
    }
    p += 46 + nlen + elen + clen;
  }
  throw new Error("no .repl26a in zip");
}

// Replay step kinds (decoded from replays; see training/README.md).
export const STEP = { PHASE: 0, RESPAWN: 1, UPDATE: 3, MOVE: 4, ATTACK: 5, ABILITY: 6, END_TURN: 8, PRIORITY: 9,
  RECALL: 11, SUBMIT_GEMS: 12, BUY: 13, WARD: 14, CONSUMABLE: 15, CANCEL_CHANNEL: 16, TRIBUTE_DONE: 18,
  DRAFT_PICK: 19, ALTAR_LOCK: 20, TELEPORT_CHANNEL: 21, TELEPORT: 22, BLOCKED: 23, RESIGN: 24, SELL: 25,
  FUEL_EXHAUSTED: 26 };

export async function loadEngine(wasmPath = findEngineWasm()) {
  let memory;
  const view = () => new DataView(memory.buffer);
  const cstr = (ptr) => {
    const b = new Uint8Array(memory.buffer);
    let e = ptr;
    while (e < b.length && b[e] !== 0) e++;
    return new TextDecoder().decode(b.subarray(ptr, e));
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
    deep_wasm: { on_assert_fail_impl: (a, f, l) => { console.error(`assert ${cstr(f)}:${l} ${cstr(a)}`); return 1; } },
  };
  const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmPath), imports);
  const x = instance.exports;
  memory = x.memory;
  x._initialize();
  return { x, view, cstr, get memory() { return memory; } };
}

// One replay loaded into the engine, positioned at the start of its main timeline.
export class Replay {
  constructor(eng, bytes) {
    this.eng = eng;
    const { x } = eng;
    this.out = x.Engine_Malloc(64);
    this.persp = x.Engine_Malloc(16);
    this.scratch = x.Engine_Malloc(4096);
    const rp = x.Engine_Malloc(bytes.length);
    new Uint8Array(eng.memory.buffer, rp, bytes.length).set(bytes);
    this.check(x.Game_FromReplayFileBytes(rp, bytes.length, this.out), "Game_FromReplayFileBytes");
    this.game = eng.view().getBigUint64(this.out, true);
    x.Engine_Free(rp);
    this.check(x.Game_Visualiser_Timeline_GetMain(this.game, this.out), "Timeline_GetMain");
    this.timeline = eng.view().getBigUint64(this.out, true);
    this.check(x.Game_Visualiser_Timeline_Info(this.game, this.timeline, this.scratch, this.scratch + 8, this.scratch + 16), "Timeline_Info");
    this.inputs = eng.view().getUint32(this.scratch, true);
    this.check(x.Game_Visualiser_Timeline_Seek(this.game, this.timeline, 0, 0), "Seek");
    const size = x.Game_Visualiser_TimelineSnapshotSize();
    if (size !== 42840) throw new Error(`snapshot size ${size}: expected engine 2.0.0 (42840)`);
    this.snap = x.Engine_Malloc(size);
    this.check(x.Game_GetBitboardMetadata(this.game, this.out), "Metadata");
    const v = eng.view();
    this.W = v.getInt32(this.out, true); this.H = v.getInt32(this.out + 4, true);
    // Board size from the chunk layout ({w, h, chunks_w, chunks_h, stride}); the metadata does not
    // carry a byte size here, and an undersized buffer silently corrupts engine memory.
    this.stride = v.getUint32(this.out + 16, true);
    const chunksH = v.getUint32(this.out + 12, true) || Math.ceil(this.H / 8);
    this.boardBytes = Math.max(this.stride, Math.ceil(this.W / 8)) * Math.max(chunksH, Math.ceil(this.H / 8)) * 8;
    this.board = (x.Engine_Malloc(this.boardBytes + 256) + 15) & ~15;
    this.list = x.Engine_Malloc(4 * 1024);
    this.readHeader();
  }

  check(code, what) {
    if (code !== 0) throw new Error(`${what} failed (${code}): ${this.eng.cstr(this.eng.x.GetLastErrorMsg())}`);
  }

  // Timeline snapshot header, plus the step that produced the current state.
  readHeader() {
    this.check(this.eng.x.Game_Visualiser_ReadTimelineState(this.game, this.snap), "ReadTimelineState");
    const v = this.eng.view(), s = this.snap, u = (o) => v.getUint32(s + o, true), i = (o) => v.getInt32(s + o, true);
    const slot = u(108);
    this.head = {
      round: u(8), outcome: u(28), awaiting: !!u(20), input: u(56),
      kind: u(68), actorKind: u(72), actor: u(76), fx: i(80), fy: i(84), tx: i(88), ty: i(92),
      slot: slot === 0xffffffff ? -1 : slot, abilityType: slot === 0xffffffff ? -1 : u(112),
      submitted: [Number(v.getBigUint64(s + 32, true)), Number(v.getBigUint64(s + 40, true))],
    };
    return this.head;
  }

  raw(offset, n) { // header words, for probing the snapshot layout
    const v = this.eng.view();
    return Array.from({ length: n }, (_, k) => v.getInt32(this.snap + offset + 4 * k, true));
  }

  finished() { return this.ended || this.head.outcome !== 0 || (this.head.awaiting && this.head.input >= this.inputs); }

  next() {
    const code = this.eng.x.Game_Visualiser_Timeline_ReplayNext(this.game, 1);
    if (code === 18) { // invalid_action: "End of timeline" (e.g. resignations end without awaiting input)
      this.ended = true;
      return this.head;
    }
    this.check(code, "ReplayNext");
    return this.readHeader();
  }

  // ---- perspective-scoped queries (what a champion's bot sees) ----
  setPerspective(team, champion) {
    const v = this.eng.view();
    v.setUint32(this.persp, team, true); v.setUint32(this.persp + 4, champion, true);
  }

  call(name, ...args) { // returns the status code instead of throwing
    return this.eng.x[name](this.game, this.persp, ...args);
  }

  u32(name, ...args) { const c = this.call(name, ...args, this.out); return c === 0 ? this.eng.view().getUint32(this.out, true) : null; }
  i32(name, ...args) { const c = this.call(name, ...args, this.out); return c === 0 ? this.eng.view().getInt32(this.out, true) : null; }

  handles(name) { // list queries: (buf, capacity, count*)
    const c = this.call(name, this.list, 1024, this.out);
    if (c !== 0) return [];
    const v = this.eng.view(), n = v.getUint32(this.out, true);
    return Array.from({ length: n }, (_, k) => v.getUint32(this.list + 4 * k, true));
  }

  champion(handle) { // champion_info: team, type, x, y, health, max_health, strength, focus, move_speed, attack_range, state
    if (this.call("Game_API_Champion_Get", handle, this.out) !== 0) return null;
    const v = this.eng.view(), o = this.out;
    return { team: v.getInt32(o, true), type: v.getInt32(o + 4, true), x: v.getInt32(o + 8, true), y: v.getInt32(o + 12, true),
      hp: v.getFloat32(o + 16, true), maxHp: v.getFloat32(o + 20, true), str: v.getFloat32(o + 24, true),
      focus: v.getFloat32(o + 28, true), range: v.getInt32(o + 36, true), state: v.getUint32(o + 40, true) };
  }

  identity(handle) {
    if (this.call("Game_API_Champion_GetIdentity", handle, this.out) !== 0) return null;
    const v = this.eng.view();
    return { team: v.getInt32(this.out, true), type: v.getInt32(this.out + 4, true) };
  }

  camp(handle) { // camp_state: x, y, health, max_health, strength, focus, cooldowns[2]
    if (this.call("Game_API_Camp_Get", handle, this.out) !== 0) return null;
    const v = this.eng.view(), o = this.out;
    return { x: v.getInt32(o, true), y: v.getInt32(o + 4, true), hp: v.getFloat32(o + 8, true), maxHp: v.getFloat32(o + 12, true) };
  }

  // Visible camps the way the SDK finds them: camp occupancy board, then camp_at(x, y) for handles.
  visibleCamps() {
    if (this.call("Game_API_GetCampOccupancy", this.board) !== 0) return [];
    const occ = this.unpack(this.board), out = [];
    for (let i = 0; i < occ.length; i++) {
      if (!occ[i]) continue;
      const x = i % this.W, y = Math.floor(i / this.W);
      if (this.call("Game_API_GetCamp", x, y, this.out) !== 0) continue;
      out.push(this.eng.view().getUint32(this.out, true));
    }
    return out;
  }

  buffs(handle) { // [{type, duration, value}]
    if (this.call("Game_API_Champion_GetBuffData", handle, this.list, 32, this.out) !== 0) return [];
    const v = this.eng.view(), n = v.getUint32(this.out, true), b = [];
    for (let k = 0; k < n; k++) {
      const o = this.list + 12 * k;
      b.push({ type: v.getUint32(o, true), duration: v.getInt32(o + 4, true), value: v.getFloat32(o + 8, true) });
    }
    return b;
  }

  items(handle, count = 84) { // per-item_type counts
    if (this.call("Game_API_Champion_GetItems", handle, this.list, count) !== 0) return null;
    const v = this.eng.view();
    return Array.from({ length: count }, (_, k) => v.getUint32(this.list + 4 * k, true));
  }

  // Engine consumable slots (4, the SDK exposes two): tp crystal, control ward, altar lock, tp scroll.
  consumables(handle) {
    if (this.call("Game_API_Champion_GetConsumables", handle, this.list, 4) !== 0) return null;
    const v = this.eng.view();
    return [0, 1, 2, 3].map((k) => v.getUint32(this.list + 4 * k, true));
  }

  wards(handle) { // ward_resources: charges, cooldown
    if (this.call("Game_API_Champion_GetWards", handle, this.out) !== 0) return null;
    const v = this.eng.view();
    return { charges: v.getUint32(this.out, true), cooldown: v.getInt32(this.out + 4, true) };
  }

  captureState(group) {
    if (this.call("Game_API_CampGroup_GetCaptureState", group, this.out) !== 0) return null;
    const v = this.eng.view();
    return { timer: v.getInt32(this.out, true), state: v.getInt32(this.out + 4, true) };
  }

  altarLocks() { // altar_lock_info: kind, handle, team, x, y, hits
    if (this.call("Game_API_GetAltarLocks", this.list, 64, this.out) !== 0) return [];
    const v = this.eng.view(), n = v.getUint32(this.out, true), r = [];
    for (let k = 0; k < n; k++) {
      const o = this.list + 24 * k;
      r.push({ team: v.getUint32(o + 8, true), x: v.getInt32(o + 12, true), y: v.getInt32(o + 16, true), hits: v.getUint32(o + 20, true) });
    }
    return r;
  }

  teleports() { // teleport_info: champion, type, team, dest x, dest y, timer, channel_type
    if (this.call("Game_API_GetTeleports", this.list, 64, this.out) !== 0) return [];
    const v = this.eng.view(), n = v.getUint32(this.out, true), r = [];
    for (let k = 0; k < n; k++) {
      const o = this.list + 28 * k;
      r.push({ champion: v.getUint32(o, true), team: v.getUint32(o + 8, true), x: v.getInt32(o + 12, true), y: v.getInt32(o + 16, true), timer: v.getInt32(o + 20, true) });
    }
    return r;
  }

  vision() { // the perspective's visible tiles as a Uint8Array (row-major, W*H)
    if (this.call("Game_API_GetVision", this.board) !== 0) return null;
    return this.unpack(this.board);
  }

  unpack(ptr) {
    const v = this.eng.view(), W = this.W, H = this.H, out = new Uint8Array(W * H);
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const a = (y >> 3) * this.stride + (x >> 3), o = ((y & 7) << 3) + (x & 7);
      const word = v.getBigUint64(ptr + a * 8, true);
      out[y * W + x] = Number((word >> BigInt(o)) & 1n);
    }
    return out;
  }

  // ---- static map data (no perspective needed) ----
  mapBoard(name) { this.check(this.eng.x[name](this.game, this.persp, this.board), name); return this.unpack(this.board); }

  campLayout() { // [{group, gems, gold, respawn, camps:[{handle, x, y, type}]}]
    const { x } = this.eng, v = () => this.eng.view(), g = this.game;
    this.check(x.Game_API_Map_GetCampGroups(g, this.list, 1024, this.out), "GetCampGroups");
    const groups = Array.from({ length: v().getUint32(this.out, true) }, (_, k) => v().getUint32(this.list + 4 * k, true));
    return groups.map((group) => {
      this.check(x.Game_API_Map_CampGroup_Get(g, group, this.out), "CampGroup_Get");
      const gems = v().getUint32(this.out, true), gold = v().getUint32(this.out + 4, true);
      const respawn = x.Game_API_Map_CampGroup_GetRespawnTime(g, group, this.out) === 0 ? v().getInt32(this.out, true) : 0;
      this.check(x.Game_API_Map_CampGroup_GetCampInfo(g, group, this.list, 1024, this.out), "CampGroup_GetCampInfo");
      const handles = Array.from({ length: v().getUint32(this.out, true) }, (_, k) => v().getUint32(this.list + 4 * k, true));
      const camps = handles.map((handle) => {
        this.check(x.Game_API_Map_CampInfo_Get(g, handle, this.out), "CampInfo_Get");
        return { handle, x: v().getInt32(this.out, true), y: v().getInt32(this.out + 4, true), type: v().getInt32(this.out + 8, true) };
      });
      return { group, gems, gold, respawn, camps };
    });
  }

  spawnRegion(team) { // aabb with exclusive max
    this.check(this.eng.x.Game_API_GetSpawnRegion(this.game, team, this.out), "GetSpawnRegion");
    const v = this.eng.view();
    return { x0: v.getInt32(this.out, true), y0: v.getInt32(this.out + 4, true), x1: v.getInt32(this.out + 8, true), y1: v.getInt32(this.out + 12, true) };
  }
}
