import { loadEngine, readReplayBytes, Replay } from "./engine.mjs";
const eng = await loadEngine();
const r = new Replay(eng, readReplayBytes(process.argv[2]));
r.setPerspective(1, 4);
const types = {0:"wolf",1:"elk",2:"gemC",3:"goldC",4:"monolith",5:"golem"};
for (const g of r.campLayout()) {
  const cs = g.camps.map(c => `${types[c.type] ?? c.type}@${c.x},${c.y}`).join(" ");
  console.log(`group ${g.group} gems=${g.gems} gold=${g.gold} respawn=${g.respawn}  ${cs}`);
}
console.log("spawnA", JSON.stringify(r.spawnRegion(0)), "spawnB", JSON.stringify(r.spawnRegion(1)));
