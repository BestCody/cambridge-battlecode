import { loadEngine, readReplayBytes, Replay, STEP } from "./engine.mjs";
const r=new Replay(await loadEngine(),readReplayBytes(process.argv[2]));const T=+process.argv[3];let h=r.head;
while(!r.finished()&&h.input<T+6){h=r.next();if(h.input>=T-6&&h.actorKind===1&&h.kind!==STEP.PHASE&&h.kind!==STEP.UPDATE)console.log(`in=${h.input} r=${h.round} actor=${h.actor} kind=${h.kind} ${h.fx},${h.fy}->${h.tx},${h.ty}`);}
