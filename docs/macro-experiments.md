# Macro experiments, 2026-11-01

What we tried on top of `baseline` to get map-specific macro, what happened, and why most of it was dropped. Everything here was measured at fuel 100,000 on the 70 eval maps in both seats (140 games) unless a smaller set is stated.

## Summary

- **Nothing beat `baseline` meaningfully.** Against `test-bot`, `baseline` (426b19c) wins 76/140 (margin +6,203). The best variant, `baseline` plus warding a hidden runner, wins 77/140 (+10,319).
- **Later, the first idea from a practice game reached `baseline`:** Marcella holds every capture she is on, so the team's gold builds one carry, as naviepstokes and dromedaries do. Over 560 paired games against `test-bot` it won 334 to 323 (gained 50, lost 39), with a better margin at all four budgets and about 1 more kill a game. Copying their emblem build as well made it worse (77-78/140 against 81). On the newest code alone it was 81/140 to 80, so the gain is small.
- **Only two earlier ideas reached `baseline`:** Vesper's early gold scouting and the detour-map crystal, adopted in 426b19c (the crystal rule was later replaced in afeac14).
- **Open bug:** on `00vault`, seat A, both teams stop acting early (ours by about round 32, `test-bot` by about round 180), and the game goes 0-0 to the tiebreak over 2,000 rounds. Neither bot faults. The cause is unknown.
- **The likeliest remaining lead is fights, not gold.** In the games we studied closely, we lost because champions were caught alone and died. Gold deficits were mostly a consequence.

## Measuring: read this before trusting a number

- **A single 140-game total moves by about ±6 wins.** The games are deterministic, but any change in a rule replays later games differently, so unrelated maps change result.
- **Compare the same map and seat.** Adding the ward rule left 63 of 140 games with the same result and margin. A change's effect is in the games it changed.
- **Measure against `bots/test-bot`, the GA-tuned one.** `baseline` wins 85/140 against the frozen pre-GA copy (`opp_testbot`) but only 76/140 against `test-bot`. Those two numbers are not comparable, and several early comparisons here used the frozen copy.
- **Don't take win counts from diagnostic builds.** A logging build scored 63/140 where the same logic without logging scored 78/140. Logging appears to change play, so use it only to explain a game. This also means our claim that the end-of-turn trace cannot affect decisions was wrong.
- **Open question:** a teammate reported a bug in the evaluation script, which we have not seen. On the 140 `baseline` games we checked, the script's wins match the engine outcome and the gem totals every time.

## Results

Against `test-bot` (140 games):

| Bot | Wins | Margin |
| --- | --- | --- |
| `baseline` (426b19c) | 76 | +6,203 |
| `baseline` + ward a hidden runner | 77 | +10,319 |
| `baseline` + ward + mid flag | 78 | +7,097 |
| `baseline` + joshua rules on 4-pick "other" maps (at the time, before the ward and mid work) | 74 | +4,715 |
| `joshua_baseline` | 67 | −188 |
| `intercont_fish_strat` | 63 | −10,672 |

`joshua_baseline` and `intercont_fish_strat` were built on an older `baseline`, without its later fight fixes, which is part of why they score lower here.

## What we tried

| Idea | Result | Decision |
| --- | --- | --- |
| Boss gems worth 1.6x when the team could survive the kill | Lost when applied to every map on the newest fight code: 82/140 with compact cohesion (paired 7:9) | Dropped |
| Keep together on compact maps | Helped only on compact maps with a boss | Dropped with the boss rule |
| Prism and Willow as priority targets | 80/140, kill ratio 0.73 | Dropped |
| Vendetta and Marcella pair up on 5 picks | 79/140 (paired 9:14) | Dropped |
| Vesper scouts gold for 300 rounds, detour-map crystal | 85/140 against 84 by a teammate's measure, better kill ratio | **In `baseline`** |
| Cassi & Issac roster on 3-4 picks | 4-pick maps 10/32 against 18/32; the twins died about 6 times a game each, and were dead 26% of turns | Dropped |
| Everyone takes the leader's camp on 3-4 picks | 9/56 with the twins roster: fewer deaths, but about a fifth fewer captures | Dropped |
| Followers farm around the leader's camp | 30/56 with the old roster, against 33/56 without following | Dropped |
| Gold, fight and gem phases | Gold-first cost about 10 wins in `macro`; the later gem discount version gained 11 wins inside `intercont_fish_strat`, but that bot stayed behind `baseline` | Dropped |
| Group fighting (stay in at 15% when backed) and waiting at spawn | Helped on 5 picks (54/84 against 47/84), hurt on twins maps | Dropped with the bot |
| Gold expert on gold-heavy maps | 15/20 without it, 11/20 and 14/20 with it; about 8% more early gold, 40-75% more deaths | Dropped |
| Mid flag (hold mid when stronger, sideline when weaker) | 20/38 on mid maps against 19/38 without it, margin about 3,200 worse | Off in `bots/merged` |
| Ward a hidden low runner | 19/38 on mid maps against 17/38 (paired 2:0); all 140 games paired 9:8 | Promising, not proven |

Rules that were clearly positive inside one test often lost when applied more widely or re-tested on newer code, so a small gain in one run is not enough to adopt a rule.

## Map classes

These classes are cheap to compute in the draft from the camp list.

| Class | Definition | Maps | Notes |
| --- | --- | --- | --- |
| Gold-heavy | 400+ gold in groups of gold clusters only | 10 | Not a weakness: `baseline` wins 15/20 |
| Career | 60%+ of gems behind elk, golem or monolith, and 200+ free gold | 18 | Not a weakness: about 60% wins |
| Gem-rush | 100 or less free gold, and more undefended gems than defended | 12 | **Weakest class**: 4-pick maps win 3/12 |
| Mid | Groups near an altar and about equally far from both spawns are worth 0.75x+ of the rest | 19 | 17/38; the flag did not help |

Team size matters too: a tiny tree of team size then archetype is enough to route a map. By that split, against `test-bot`, no alternative was significantly better than `baseline` in any class, so no mixture of experts is justified yet.

## What close study of single games showed

Reading one game turn by turn explained far more than totals did.

- **Intercontinental Fish on `00quarry` (beat Mite with the same draft):** gold on their own side first, regroup all five, and hit Mite while it was busy clearing the 46-gem centre. They died 3 times to Mite's roughly 30, and banked small bags (14-46 gems) often.
- **Their shopping adapts from game to game:** farming emblems usually first (Capture, Prospector, Treasure, Jungle, Monster), combat emblems later, and frequent altar locks. Cassi is the exception and goes combat first.
- **`adgato-gemcombat1`:** we led until round 500, then died 11 times without a kill. At each death all three enemies were within 1-5 tiles, and our teammates were usually 6-24 tiles away. Their Marcella reached 2,200-2,900 health while ours stayed near 1,000.
- **`01elk-arena` against naviepstokes:** 0 gems, 28 deaths against 1, and 2,147 gold to 10,365. Most of the gold gap came from kills and holding the centre. Discounting gems to send champions to the outskirts did not help against `test-bot`, which does not play this way, so that game cannot be tested locally.
- **`01hooks` against `test-bot`:** we led in gold until about round 500, then collected 23 gems to their 169 by round 700 while they held the centre.
- **Hidden runners escape:** on `02arena` and `01hooks`, low enemies stood in a bush 3-4 tiles away while our chaser flipped between chasing them and walking to its camp, until they recalled.

## Suggested next steps

1. Find out why champions get caught alone on gem-rush maps, and why our health stays so low there.
2. Re-test the ward rule on `baseline`, comparing the same map and seat.
3. Use `docs/baseline-ablations.md` to see which of `baseline`'s rules matter before adding more.
