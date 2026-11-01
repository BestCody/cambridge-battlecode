// One line per champion per round (state at the round's end, from its own view): position, health, gems,
// and the actions it took that round. usage: node rounds.mjs replay.repl26a [EVERY=1] > out.txt
import { loadEngine, readReplayBytes, Replay, STEP } from "./engine.mjs";
const [file, everyArg] = process.argv.slice(2);
const every = Number(everyArg ?? 1);
const TYPES = ["Ryth", "Vesper", "Vendetta", "Cassi", "Issac", "Stellar", "Prism", "Marcella", "Willow", "Ren", "Varg", "Tavi", "HoR"];
const eng = await loadEngine();
const r = new Replay(eng, readReplayBytes(file));
const ids = new Map(); // handle -> {team, type}
function learn(h) {
  if (ids.has(h)) return;
  for (const t of [0, 1]) { r.setPerspective(t, h); const id = r.identity(h); if (id) { ids.set(h, id); return; } }
}
const acts = new Map();
let round = r.head.round;
const gemsOf = (h) => {
  const c = r.call("Game_API_Champion_GetGems", h, r.out);
  return c === 0 ? r.eng.view().getUint32(r.out, true) : -1;
};
function dump(rnd) {
  if (rnd % every) return;
  const parts = [];
  for (const [h, id] of ids) {
    r.setPerspective(id.team, h);
    const c = r.champion(h);
    const a = acts.get(h) ?? "";
    if (!c) { parts.push(`${TYPES[id.type]}${"AB"[id.team]}=dead`); continue; }
    parts.push(`${TYPES[id.type]}${"AB"[id.team]}=${c.x},${c.y} hp${c.hp.toFixed(0)} g${gemsOf(h)} s${c.state.toString(16)}${a ? " " + a : ""}`);
  }
  const sub = r.head.submitted;
  console.log(`r${rnd} A${sub[0]} B${sub[1]} | ${parts.join(" | ")}`);
}
const NAME = { [STEP.MOVE]: "mv", [STEP.ATTACK]: "atk", [STEP.ABILITY]: "abl" };
while (!r.finished()) {
  const h = r.next();
  if (h.round !== round) {
    dump(round);
    acts.clear();
    round = h.round;
  }
  if (h.actorKind === 1) learn(h.actor);
  if (h.actorKind === 1 && NAME[h.kind]) {
    const tag = NAME[h.kind] + (h.kind === STEP.ABILITY ? h.slot : "") + `>${h.tx},${h.ty}`;
    acts.set(h.actor, (acts.get(h.actor) ? acts.get(h.actor) + " " : "") + tag);
  }
}
dump(round);
