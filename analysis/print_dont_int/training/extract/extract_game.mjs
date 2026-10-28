// Re-simulate one replay and write, for every turn of the target team's champions, what that
// champion's bot could observe at the start of its turn (read through the engine's own
// perspective-scoped Game_API_* queries, so vision is enforced by the engine) plus every action it
// took that turn.
//
// usage: node extract_game.mjs <replay.zip> <team 0|1> <out.jsonl.gz> [match id]
//
// Output: gzipped JSON lines. Line 1 is the game header (map, camps, champions); each following line
// is one champion turn: {r, c, obs, steps:[{econ, act}], end}. See ../README.md for the schema.
import fs from "node:fs";
import zlib from "node:zlib";
import { loadEngine, Replay, readReplayBytes, STEP } from "./engine.mjs";

const [replayPath, teamArg, outPath, matchId = ""] = process.argv.slice(2);
const TEAM = +teamArg;
const eng = await loadEngine();
const rp = new Replay(eng, readReplayBytes(replayPath));
const v = () => eng.view();

// Champion roster from the timeline snapshot (ids, teams, types; static for the game).
function roster() {
  const s = rp.snap, n = v().getUint32(s + 172, true), out = [];
  for (let k = 0; k < n; k++) {
    const b = 196 + k * 420;
    out.push({ id: v().getUint32(s + b + 4, true), team: v().getUint32(s + b + 8, true), type: v().getUint32(s + b + 12, true) });
  }
  return out;
}
const PHASE = () => v().getUint32(rp.snap + 24, true);
const CURRENT = () => v().getInt32(rp.snap + 60, true);

// Skip the draft until champions exist on the board.
while (!rp.finished() && roster().length === 0) rp.next();
const champs = roster();
const ours = champs.filter((c) => c.team === TEAM).map((c) => c.id);

// ---- static header ----
rp.setPerspective(TEAM, ours[0]);
const rows = (name) => {
  const b = rp.mapBoard(name), r = [];
  for (let y = 0; y < rp.H; y++) r.push(Array.from(b.subarray(y * rp.W, (y + 1) * rp.W)).join(""));
  return r;
};
const header = {
  matchId, team: TEAM, W: rp.W, H: rp.H,
  symmetry: eng.x.Game_API_Map_GetSymmetry(rp.game, rp.out) === 0 ? v().getInt32(rp.out, true) : -1,
  walls: rows("Game_API_Map_GetWallBitboard"), bushes: rows("Game_API_Map_GetBushBitboard"),
  altars: rows("Game_API_Map_GetAltarBitboard"), holes: rows("Game_API_Map_GetHoleBitboard"),
  spawns: [rp.spawnRegion(0), rp.spawnRegion(1)],
  groups: rp.campLayout(),
  champions: champs,
};

// ---- per-turn observation, from champion c's perspective ----
const allyIds = champs.filter((c) => c.team === TEAM).map((c) => c.id);
const enemyIds = champs.filter((c) => c.team !== TEAM).map((c) => c.id);
const nz = (arr) => { const o = {}; arr?.forEach((n, i) => { if (n) o[i] = n; }); return o; };
const round1 = (f) => Math.round(f * 10) / 10;

function unit(id) {
  const ch = rp.champion(id);
  const alive = eng.x.Game_API_Champion_IsAlive(rp.game, id, rp.out) === 0 ? v().getInt32(rp.out, true) : -1;
  if (!ch) return { id, vis: 0, alive };
  const buffs = rp.buffs(id);
  const stun = buffs.find((b) => b.type === 1), shield = buffs.find((b) => b.type === 0);
  return { id, vis: 1, alive, type: ch.type, x: ch.x, y: ch.y, hp: round1(ch.hp), mhp: round1(ch.maxHp), str: round1(ch.str), foc: round1(ch.focus),
    st: ch.state, gems: rp.u32("Game_API_Champion_GetGems", id), stun: stun ? stun.duration : 0,
    shield: shield ? round1(shield.value) : 0, chan: rp.i32("Game_API_Champion_GetChannelTime", id) };
}

function econ(c) {
  return { x: rp.champion(c)?.x, y: rp.champion(c)?.y, gold: rp.u32("Game_API_Champion_GetGold", c),
    gems: rp.u32("Game_API_Champion_GetGems", c), items: nz(rp.items(c)), cons: rp.consumables(c),
    cd: [rp.i32("Game_API_Champion_GetCooldown", c, 0), rp.i32("Game_API_Champion_GetCooldown", c, 1)],
    st: rp.champion(c)?.st ?? rp.champion(c)?.state };
}

function observe(c) {
  rp.setPerspective(TEAM, c);
  const self = unit(c);
  const wards = rp.wards(c);
  self.ward = wards ? [wards.charges, wards.cooldown] : null;
  self.wardPos = (() => {
    if (rp.call("Game_API_Champion_GetWardPositions", c, rp.list, 8, rp.out) !== 0) return [];
    const n = v().getUint32(rp.out, true);
    return Array.from({ length: n }, (_, k) => [v().getInt32(rp.list + 8 * k, true), v().getInt32(rp.list + 8 * k + 4, true)]);
  })();
  self.resp = eng.x.Game_API_Champion_GetRespawnTimer(rp.game, c, rp.out) === 0 ? v().getInt32(rp.out, true) : 0;
  self.prio = rp.call("Game_API_Champion_GetPriority", c, 1, rp.out) === 0 ? v().getInt32(rp.out, true) : 0;
  self.restr = rp.call("Game_API_Champion_GetRestrictions", c, rp.out) === 0 ? v().getUint8(rp.out) : 0;
  const sub = [TEAM, 1 - TEAM].map((t) => (eng.x.Game_API_GetSubmittedGemCount(rp.game, t, rp.out) === 0 ? v().getUint32(rp.out, true) : 0));
  const camps = rp.visibleCamps().map((h) => {
    const cs = rp.camp(h);
    return cs && [h, cs.x, cs.y, round1(cs.hp), round1(cs.maxHp), rp.u32("Game_API_Camp_GetState", h)];
  }).filter(Boolean);
  const groups = [];
  for (const g of header.groups) {
    const state = rp.u32("Game_API_CampGroup_GetState", g.group);
    if (!state) continue; // not in vision
    const cap = rp.captureState(g.group);
    groups.push([g.group, state, state === 3 ? rp.i32("Game_API_CampGroup_GetCooldown", g.group) : 0, cap ? cap.state : -1, cap ? cap.timer : -1]);
  }
  return {
    self, ...econ(c), sub,
    allies: allyIds.filter((id) => id !== c).map(unit),
    enemies: enemyIds.map(unit),
    camps, groups,
    locks: rp.altarLocks(), tps: rp.teleports(),
  };
}

// ---- walk the timeline ----
const out = [];
let turn = null; // {r, c, obs, steps:[{econ, act}]}
const flush = () => { if (turn) out.push(JSON.stringify(turn)); turn = null; };
let guard = 0, truncated = null;
while (!rp.finished()) {
  const h = rp.head;
  if (h.awaiting && PHASE() === 10) {
    const c = CURRENT();
    if (ours.includes(c)) {
      if (!turn || turn.c !== c || turn.r !== h.round) {
        flush();
        turn = { r: h.round, c, obs: observe(c), steps: [] };
      } else {
        rp.setPerspective(TEAM, c);
        turn.pending = econ(c); // state before the next action
      }
    }
  }
  let s;
  try {
    s = rp.next();
  } catch (e) { // the engine rejected a recorded action (replay desync): keep what came before it
    truncated = String(e.message || e).slice(0, 200);
    break;
  }
  if (rp.ended) break;
  if (++guard > 5e6) throw new Error("runaway replay");
  if (turn && s.actorKind === 1 && s.actor === turn.c && s.round === turn.r && s.kind !== STEP.UPDATE) {
    if (s.kind === STEP.END_TURN || s.kind === STEP.FUEL_EXHAUSTED) {
      turn.end = s.kind; delete turn.pending; flush();
      continue;
    }
    const act = { k: s.kind, fx: s.fx, fy: s.fy, tx: s.tx, ty: s.ty };
    if (s.slot >= 0) { act.slot = s.slot; act.at = s.abilityType; }
    if (s.kind === STEP.BUY || s.kind === STEP.SELL || s.kind === STEP.PRIORITY) {
      rp.setPerspective(TEAM, turn.c);
      act.after = { gold: rp.u32("Game_API_Champion_GetGold", turn.c), items: nz(rp.items(turn.c)), cons: rp.consumables(turn.c),
        prio: rp.call("Game_API_Champion_GetPriority", turn.c, 1, rp.out) === 0 ? v().getInt32(rp.out, true) : 0 };
    }
    turn.steps.push(turn.pending ? { econ: turn.pending, act } : { act });
    delete turn.pending;
  }
}
flush();
const final = { outcome: rp.head.outcome, round: rp.head.round, submitted: rp.head.submitted, truncated };
const text = [JSON.stringify({ header, final })].concat(out).join("\n") + "\n";
fs.writeFileSync(outPath + ".part", zlib.gzipSync(text, { level: 6 }));
fs.renameSync(outPath + ".part", outPath);
console.log(JSON.stringify({ turns: out.length, ...final }));
