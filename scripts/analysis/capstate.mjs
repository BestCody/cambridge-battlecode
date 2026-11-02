// Capture state of a camp group each round, from an owned champion's view: node capstate.mjs replay GROUP [FROM TO]
import { loadEngine, readReplayBytes, Replay } from "./engine.mjs";
const [file, g, from = "0", to = "200"] = process.argv.slice(2);
const eng = await loadEngine();
const r = new Replay(eng, readReplayBytes(file));
let handle = null;
let round = r.head.round, last = "";
while (!r.finished()) {
  const h = r.next();
  if (handle === null) for (const hd of [h.handle, h.champion, h.actor].filter(v => v !== undefined))
    for (const t of [0, 1]) { r.setPerspective(t, hd); if (r.identity(hd)) { handle = hd; break; } }
  if (h.round !== round) {
    if (handle !== null && round >= +from && round <= +to) {
      const s = JSON.stringify(r.captureState(+g));
      if (s !== last) { console.log(`r${round} ${s}`); last = s; }
    }
    round = h.round;
    if (round > +to) break;
  }
}
