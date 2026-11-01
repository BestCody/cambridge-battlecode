// Print every replay step by chosen champions within an input range, with their state.
// usage: node trace.mjs replay.repl26a FROM TO champ[,champ...]
import { loadEngine, readReplayBytes, Replay, STEP } from "./engine.mjs";
const [file, from, to, who] = process.argv.slice(2);
const champs = new Set(who.split(",").map(Number));
const names = Object.fromEntries(Object.entries(STEP).map(([k, v]) => [v, k]));
const eng = await loadEngine();
const r = new Replay(eng, readReplayBytes(file));
let h = r.head;
while (!r.finished() && h.input < Number(to)) {
  h = r.next();
  if (h.input < Number(from)) continue;
  if (h.actorKind !== 1 && h.kind !== STEP.END_TURN) { /* actorKind 1 = champion? print anyway when actor matches */ }
  if (!champs.has(h.actor) || h.kind === STEP.PHASE || h.kind === STEP.UPDATE) continue;
  let extra = "";
  if (h.kind === STEP.END_TURN) {
    r.setPerspective(1, h.actor);
    const c = r.champion(h.actor);
    if (c) extra = ` pos=${c.x},${c.y} hp=${c.hp.toFixed(0)}/${c.maxHp.toFixed(0)} state=${c.state.toString(16)}`;
  }
  console.log(`in=${h.input} r=${h.round} champ=${h.actor} ak=${h.actorKind} ${names[h.kind] ?? h.kind} from=${h.fx},${h.fy} to=${h.tx},${h.ty} slot=${h.slot}${extra}`);
}
