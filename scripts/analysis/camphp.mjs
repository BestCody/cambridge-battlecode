// Health of the camp at (X,Y) each round it is in some champion's view: node camphp.mjs replay X Y [FROM TO]
import { loadEngine, readReplayBytes, Replay } from "./engine.mjs";
const [file, X, Y, from = "0", to = "2000"] = process.argv.slice(2);
const eng = await loadEngine();
const r = new Replay(eng, readReplayBytes(file));
let round = r.head.round, last = "", handles = new Set();
while (!r.finished()) {
  const h = r.next();
  for (const hd of [h.handle, h.champion, h.actor].filter(v => v !== undefined)) handles.add(hd);
  if (h.round !== round) {
    if (round >= +from && round <= +to) {
      let best = null;
      for (const hd of handles) for (const t of [0, 1]) {
        r.setPerspective(t, hd); if (!r.identity(hd)) continue;
        if (r.call("Game_API_GetCamp", +X, +Y, r.out) !== 0) continue;
        const camp = r.camp(r.eng.view().getUint32(r.out, true));
        if (camp) { best = camp; break; }
      }
      const s = best ? `${best.hp}/${best.maxHp}` : "unseen";
      if (s !== last) { console.log(`r${round} ${s}`); last = s; }
    }
    round = h.round; if (round > +to) break;
  }
}
