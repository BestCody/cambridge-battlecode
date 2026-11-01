// Map building over the engine's MapBuilder exports (what `cambc mapeditor`
// runs on), for scripts: load/inspect/create/encode .map26a maps.
import fs from "node:fs";

export const CAMP = { wolf: 0, elk: 1, gem_cluster: 2, gold_cluster: 3, monolith: 4, golem: 5 };
export const CAMP_NAME = ["wolf", "elk", "gem_cluster", "gold_cluster", "monolith", "golem"];
export const SYMMETRY = { horizontal: 0, vertical: 1, rotational180: 2 };

export async function loadEngine(wasmPath) {
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
  const out = x.Engine_Malloc(64);
  const check = (code, what) => {
    if (code !== 0) throw new Error(`${what} failed (${code}): ${cstr(x.GetLastErrorMsg())}`);
  };
  const u32 = (o) => view().getUint32(out + o, true);
  const i32 = (o) => view().getInt32(out + o, true);
  const u64 = (o) => view().getBigUint64(out + o, true);

  const B = {
    fromFile(path) {
      const bytes = fs.readFileSync(path);
      const p = x.Engine_Malloc(bytes.length);
      new Uint8Array(memory.buffer, p, bytes.length).set(bytes);
      try { check(x.MapBuilder_FromFileBytes(p, bytes.length, out), "FromFileBytes"); } finally { x.Engine_Free(p); }
      return u64(0);
    },
    create(width, height, symmetry) {
      check(x.MapBuilder_Create(width, height, symmetry, out), "Create");
      return u64(0);
    },
    destroy(h) { x.MapBuilder_Destroy(h); },
    add(h, kind, px, py) {
      const f = { wall: x.MapBuilder_AddWall, bush: x.MapBuilder_AddBush, hole: x.MapBuilder_AddHole,
                  altar: x.MapBuilder_AddAltar }[kind];
      check(f(h, px, py), `Add ${kind} ${px},${py}`);
    },
    remove(h, kind, px, py) {
      const f = { wall: x.MapBuilder_RemoveWall, bush: x.MapBuilder_RemoveBush, hole: x.MapBuilder_RemoveHole,
                  altar: x.MapBuilder_RemoveAltar }[kind];
      check(f(h, px, py), `Remove ${kind} ${px},${py}`);
    },
    addCamp(h, kind, px, py) {
      check(x.MapBuilder_AddCamp(h, kind, px, py, out), `AddCamp ${CAMP_NAME[kind]} ${px},${py}`);
      return u64(0);
    },
    setSpawn(h, minX, minY, maxX, maxY) {
      const v = view();
      v.setInt32(out, minX, true); v.setInt32(out + 4, minY, true);
      v.setInt32(out + 8, maxX, true); v.setInt32(out + 12, maxY, true);
      check(x.MapBuilder_SetTeamASpawnArea(h, out), "SetTeamASpawnArea");
    },
    setChamps(h, n) { check(x.MapBuilder_SetNumChampsPerTeam(h, n), "SetNumChampsPerTeam"); },
    verify(h) { check(x.MapBuilder_VerifyIntegrity(h), "VerifyIntegrity"); },
    // Load encoded map bytes as a game, as a match does: null if it loads,
    // else the engine's error.
    gameLoadError(bytes) {
      const p = x.Engine_Malloc(bytes.length);
      new Uint8Array(memory.buffer, p, bytes.length).set(bytes);
      try {
        const code = x.MapBuilder_Destroy ? x.Game_FromMapFileBytes(p, bytes.length, out, 0) : -1;
        if (code !== 0) return `code ${code}: ${cstr(x.GetLastErrorMsg())}`;
        x.Game_Destroy(u64(0));
        return null;
      } finally { x.Engine_Free(p); }
    },
    encode(h) {
      B.verify(h);
      check(x.MapBuilder_Encode(h, out, out + 4), "Encode");
      const p = u32(0), n = u32(4);
      return Buffer.from(new Uint8Array(memory.buffer, p, n));
    },
    // Full-map terrain as row strings: '#' wall, '"' bush, 'o' hole, 'A' altar,
    // '.' ground (bitboards in 8x8 chunks, as the map editor reads them).
    terrain(h) {
      const bufs = {};
      for (const [k, f] of [["bush", x.MapBuilder_GetBushBuffer], ["wall", x.MapBuilder_GetWallBuffer],
                            ["hole", x.MapBuilder_GetHoleBuffer], ["altar", x.MapBuilder_GetAltarBuffer]]) {
        check(f(h, out), `Get ${k} buffer`); bufs[k] = u32(0);
      }
      check(x.MapBuilder_GetBitboardMetadata(h, out), "GetBitboardMetadata");
      const W = i32(0), H = i32(4), stride = u32(16);
      const test = (buf, px, py) => {
        const word = (py >> 3) * stride + (px >> 3), bit = ((py & 7) << 3) + (px & 7);
        return (view().getBigUint64(buf + word * 8, true) & (1n << BigInt(bit))) !== 0n;
      };
      const rows = [];
      for (let py = 0; py < H; py++) {
        let s = "";
        for (let px = 0; px < W; px++)
          s += test(bufs.wall, px, py) ? "#" : test(bufs.hole, px, py) ? "o" : test(bufs.altar, px, py) ? "A"
             : test(bufs.bush, px, py) ? '"' : ".";
        rows.push(s);
      }
      return { W, H, rows };
    },
    info(h) {
      const r = {};
      check(x.MapBuilder_GetSymmetry(h, out), "GetSymmetry"); r.symmetry = i32(0);
      check(x.MapBuilder_GetNumChampsPerTeam(h, out), "GetNumChamps"); r.champs = u32(0);
      check(x.MapBuilder_GetTeamASpawnArea(h, out), "GetSpawn");
      r.spawnA = { minX: i32(0), minY: i32(4), maxX: i32(8), maxY: i32(12) };
      check(x.MapBuilder_GetNumCamps(h, out), "GetNumCamps"); const nc = u32(0);
      r.camps = [];
      for (let i = 0; i < nc; i++) {
        check(x.MapBuilder_GetCampHandle(h, i, out), "GetCampHandle"); const ch = u64(0);
        check(x.MapBuilder_GetCamp(h, ch, out), "GetCamp");
        r.camps.push({ kind: CAMP_NAME[i32(0)] ?? i32(0), x: i32(4), y: i32(8) });
      }
      check(x.MapBuilder_GetNumCampGroups(h, out), "GetNumCampGroups"); const ng = u32(0);
      r.groups = [];
      for (let i = 0; i < ng; i++) {
        check(x.MapBuilder_GetCampGroupHandle(h, i, out), "GetCampGroupHandle"); const gh = u64(0);
        check(x.MapBuilder_GetCampGroup(h, gh, out), "GetCampGroup");
        r.groups.push({ gems: u32(0), gold: u32(4), cooldown: u32(8), camps: u32(12) });
      }
      return r;
    },
  };
  return B;
}
