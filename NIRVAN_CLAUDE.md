# NIRVAN_CLAUDE.md

Handoff for a new Claude instance on this repo, written 2026-11-01. It covers what a long session learned the hard way. Check anything that can change (versions, ranks, results) before relying on it.

## The project

- Cambridge Battlecode 2026: 5v5-style champions on 70+ maps. First team to tribute 1,000 gems wins, otherwise most gems at round 2,000. Rules: `docs/GameRules.md`. API: `docs/BotAPICpp.md`.
- Our team on the ladder: **The Party of Heroes** (rank 45-49 at last check). Top teams: Intercontinental Fish, newer team, Mite, NEETCoders, print dont int, T1.
- Teammates work on the same repo and push often. "Dot [FATE]" / Anson owns much of `bots/baseline` and runs a GA in `scripts/ga`. The user is Joshua.
- `bots/baseline` is the main bot, `bots/test-bot` the reference opponent. `bots/starter-cpp` is the template. Everything else was deleted.

## How to work with the user

**Roles:** Claude has ideas, implements and tests. The user watches games in the visualiser and spots what looks wrong. Trust their eye, but check it against the data and say so when it disagrees. They asked to be challenged.

**Study one game very carefully, not a corpus.** The user was emphatic, and they are right: every concrete finding came from stepping through a single game. Headlines say whether we're doing well, never why.

- **Keep runs to about 140 games.** Don't throw compute at it. Prefer simple changes to the bot algorithm.
- **Practice matches against teams above us are the most valuable thing** (below). Use the API yourself.
- **Keep working autonomously** when asked, and report honestly when an idea fails. Most ideas failed.
- **Give copy-paste commands**, with the Windows `cmd` shell in mind: no `#` comments, no bash-only syntax.
- **Don't commit, push, submit or activate unless asked.** The user usually wants the commands, and sometimes asks Claude to do it. Practice matches were explicitly allowed.
- **When merging bots:** classify maps with a simple tree (team size, then 1-2 features) and route each class to the significantly better bot. Keep the classifier cheap in code and fuel.

## Commands

```
python scripts/evaluate.py --bot baseline --baseline test-bot --fuels 100000
python scripts/evaluate.py --bot baseline --baseline test-bot --fuels 100000 --out replays/x plain:some_constant=0.0f current
python scripts/evaluate.py --bot baseline --baseline test-bot --fuels 100000 --maps 00vault,01hooks
cambc view replays/x/current-test-bot-100000-B-00quarry.repl26a
cambc submit baseline -n "note"
cambc submissions list
cambc submissions activate v78
```

- **A config** is `NAME:constant=value,...` and patches an `inline constexpr` policy value in `strategy.h`. This is the cheap way to test a switch.
- **140 games** take about 2 minutes on an idle machine and much longer alongside other batches. Results go in `games.json` in the `--out` folder.
- **Pinned or following:** check the mode in `cambc submissions list`. If pinned, submitting does not change what's live. Activate a number to pin, or `latest` to follow.

### Practice matches and ladder data

```
cambc ladder --json
cambc match-groups quota --json
cambc match-groups create <team-id> --maps "00quarry,adgato_gemcombat3,01hooks,02arena,00corners" --sides a,b,a,b,a --json
cambc match-groups show <group-id> --json
cambc matches list --team "Intercontinental Fish" --kind ranked --limit 100 --json
cambc matches summary <match-id> --json -o out.json
cambc matches download <match-id> -o out.zip
```

- A group is 5 games, usually done in under a minute, with a quota of 50 groups per period. Platform map names use underscores (`adgato_gemcombat3`, `01elk_arena`).
- **Practice matches play our live version only.** Testing new code means it has to go live first.
- `quota --json` also gives `opponentRanks` by team id, which is huge: never print it whole.
- Team ids: Intercontinental Fish `1890540d-1ffc-4847-adbf-d0d08772f268`, Mite `ee83f5e5-bcad-4923-86b4-b79ea6083d2e`, naviepstokes `aab88deb-381c-4dad-a439-20e8695fa132`, dromedaries `40cf616f-378f-48c7-b381-fe323bce3773`, T1 `f6e2d7fe-9ae5-44b6-b4ad-a79d2443cb89`, us `c2892f58-a3ad-4ff7-a256-120445ad6ee7`.
- Set `PYTHONIOENCODING=utf-8` and open JSON with `encoding="utf-8"`: some team names break cp1252.

## Understanding a game

Stats JSON: `championTimelines` (running totals as runs of `{duration, value}`; metrics include `earnedGold`, `collectedGems`, `submittedGems`, `kills`, `deaths`, `championDamageDone`, `healingDone`, `spentGold`), `events` (by `kind`: `death`, `kill`, `gemSubmission`, `gemCollection`, `campClear`, `itemCombination`, `buyItem`, `gemLoss`, `fuelExhausted`), `roster`, `teams`, `match`. A `.repl26a` from a local run is a zip holding `replay.repl26a` and each team's stdout. A downloaded match is a zip too.

Tools are in `scripts/analysis/`. They run the visualiser's own engine, found by `engine.mjs`. Run the `.mjs` tools from that folder:

| Tool | Use |
| --- | --- |
| `node layout.mjs replay.repl26a` | Camp groups (camps, gems, gold, respawn) and spawn regions |
| `node rounds.mjs replay.repl26a [every]` | Each champion each round: position, health, gems, actions. The most useful one |
| `python narrate.py rounds.txt layout.txt A\|B FROM TO [summary.json]` | English play-by-play per champion |
| `node inputround.mjs replay.repl26a INPUT` | Actions around a visualiser action number |
| `node trace.mjs replay.repl26a FROM TO champ,champ` | Chosen champions' steps in an input range |
| `qparse.py` | `load(rounds.txt)` for your own analysis |
| `python firstpass.py summary.json` | The first pass below in one go: team timeline, champion totals, emblems, deaths with killers, banks |
| `python bags.py rounds.txt [min]` | Champions carrying a constant bag for min+ rounds (where it went, whether it died) |
| `node mapgrid.mjs replay.repl26a` | The map as text: walls, holes, bushes, altars, camps, spawns |
| `node capstate.mjs replay.repl26a GROUP [FROM TO]` | The engine's capture state of a group over rounds (timer stuck = contested) |

Local replays also carry our stdout (`team-a.stdout` in the `.repl26a` zip, NUL-padded): a `diagnostic=true` config prints STATE/ACTIVITY lines, and CAP/HOLD/SWEEP/STANDOFF/TRIB lines added on 2026-11-01.

**A good first pass on a game:** the timeline of gold, banked gems and deaths at rounds 100/300/500/800; gold and kills per champion; emblem order from `itemCombination`; and for each of our deaths, who was how far away. That table alone found the Marcella carry.

**Narrator caveats:** "IDLE" means no move or action that round, which includes channelling. "walking near" names the closest camp, not the target. Check raw actions before believing it: once it made a ranged champion attacking every round look idle.

## Measuring: mistakes already made

- **Measure against `bots/test-bot`.** The frozen pre-GA copy gave 85/140 where the live one gave 76, and comparing across the two caused real confusion.
- **A 140-game total moves about ±6 wins.** Games are deterministic, but any rule change shifts later play on many maps. An ablation scored 63 on one commit and 83 on the next.
- **Compare the same map and seat** (games only this version won, against only the other), and look at margin, kills and deaths too. Validate on other fuel budgets, such as `--fuels 96000,104000`, before believing a gain.
- **Never take win counts from a diagnostic build.** A logging build scored 63 where the same logic scored 78. Use logging only to explain a game, and confirm the game replays identically first. Printing can cost fuel and diverge the game.
- **Freeze a copy of the bots before a long batch.** `evaluate.py` copies `bots/` fresh per config, so a `git pull` mid-batch silently changes the code under test. This happened once.
- **Don't run batches in parallel without lowering `--jobs`.** Keep `--out` inside the repo: `cambc run-batch` splits manifest lines on spaces and the workspace path has one (the "missing result" failure).
- **Bash heredocs turn `\n` inside C++ strings into real newlines.** Use the Write tool for scripts that generate code.
- **Ablation is mostly hindsight:** it shows which existing rules matter, not what's missing.

## Findings

Details: `docs/macro-experiments.md`, `docs/baseline-ablations.md`.

**State (2026-11-02):** `baseline` with the teammate's gold weighting (aa2abcc) wins 87/140 against `test-bot`; with the contested-capture rules below, 88 (paired +4 -3). The target the user wants is 100+/140. The user has authorised committing and deploying any change that helps even on some maps, and prefers mixture-of-experts routing by map class.

**Practice matches, v78 (2026-11-01 evening): 0 of 10** against Intercontinental Fish and Mite on gemfight, elk_arena, corners, vault and gemcombat1, every game lost by their 1,000 gems. Summaries in `analysis/pg4/summ`, replays in `analysis/pg4/m_*`. What the games showed:
- **00corners against Fish: we banked 0 gems in 1,200 rounds** while collecting 292 and earning 2,809 gold. Fish banked 67 times (8-39 gems a time) at the centre altar, which their whole team sat on. Ours carried bags of 4-34 gems for 70-340 rounds and died with them; the corner altars (14 steps from a capture) were never used, and from spawn the bot heads to the centre altar and meets the group there.
- **00gemfight against Fish:** the centre is inside a spiral of holes; our melee walk 25-33 rounds to reach it (Heart of Ruin dashes over the hole line in 14), while Fish clears the wolf+gold camp beside its spawn with all five, then the elk group, and arrives at the centre at round 34 with 538 gold at round 100 to our 80. We took the 78-gem capture at round 43 under their attack, and Heart of Ruin recalled home with 39 gems, left spawn with them still in the bag, chased a low enemy, and died at round 115 at their elk camp.
- **00vault against Mite:** Mite's Marcella captured the whole 343-gem vault in one capture at round 60 and teleported home. Our melee stood idle outside the wall under Cassi's fire. Against test-bot in seat A the game froze 0-0 to round 2,000: both bots held tiles of the vault five apart with the enemy in view, each bot's tile scoring keeping its last safe tile (our ACTIVITY log: fight=1971 turns). Fixed by `standoff_rounds` (below). In seat B our Vesper carried 401 gems to the altar and died 21 turns into the 41-turn tribute.
- The tribute channel is a turn per 10 gems and there is no partial tribute: big bags are the problem, not the walk. But `bank_bag` (bank at 40 or 80 gems) lost against test-bot (74 and 85/140): the trips cost deaths. Left in at 100000 (off).

**What top teams do** (naviepstokes, dromedaries, Intercontinental Fish):
- **One carry.** Marcella holds nearly all captures (up to 71% of team gold) and stacks Bruiser early: Treasure or Home, Jungle, Durability, then Bruiser repeatedly. She reaches 3,000 health and lands most kills.
- **Move as a group.** At our deaths all their champions are within about 5 tiles and ours are 7-20 away. They punish a team busy clearing a big camp, then split to take the reward.
- **Heal.** Their Willow's damage is tiny, but she healed 11,816 in one game (`00gemfight`). Judge support champions by `healingDone`.
- **Intercontinental Fish rosters:** 3 picks Vesper/Marcella/C&I or Vesper/HoR/C&I, 4 picks Marcella/Vendetta/HoR/C&I, 5 picks adds Willow. Their shopping varies by game, usually farming emblems first.

**What worked:** Marcella holding every capture she's on (paired 50:39 over 560 games, but only 81 to 80 on newest code). Earlier, Vesper's gold scouting (a teammate later found removing it may not hurt).

**What failed when tested, with no need to repeat as is:**
- A round-1 monolith rush: five fresh champions take about 16,000 damage against about 5,000 health.
- Copying their Bruiser build order alone: 77/140 to 81. The bot may need more than the emblem order, because their carry's survival depends on the group and the healer.
- Sharing captures evenly or to the poorest: 58-60/140 to 81.
- Willow rosters on 4 picks: 9-13/32 to 17. Our bot may not use a healer well, so this doesn't prove healers are bad.
- Cassi & Issac: they die about 6 times a game each, because the bot plays them as independent champions.
- Follow-the-leader: strict follow cost a fifth of captures; a loose leash was neutral.
- Gold-first phases; gold-heavy expert (gem discounts raise deaths); mid-control flag; boss gem weight and compact cohesion; Prism/Willow priority; Vendetta+Marcella pairing.
- Closing or leaving when shot with no strike available: 72/140 to 81.
- `scarce_gold_boost` 1.5 and 2.0 (gold worth more on maps with 800 or less camp gold): 89 and 87 to 87. Nothing.
- `bank_nearby` 6 and 12 (bank whenever an altar is that close): 88 and 74 to 87, deaths up 19%.
- `bank_bag` 40 and 80 (go bank at that bag size): 74 and 85 to 88.

**Added 2026-11-02 (neutral against test-bot, 88/140):** `sweep_contested` reads the engine's `capture_state()` on a held capture, so an enemy on the tiles out of view counts as contesting, and the holder walks the tiles toward the enemy spawn and back; `standoff_rounds` closes on an enemy in view on a contested capture after 6 rounds of nobody moving. Vault seat A went from 0-0 at round 2,000 to 343-1037 (test-bot's Prism took the next three vault spawns, 50 rounds apart, while we went home to bank).

**What matters most in `baseline`** (ablation): recall home after a capture (57/140 without it), the long-plan penalty (64), retreating at low health (67), capture holder order, and Prospector and Durability emblems.

**Map classes** (cheap in the draft, from the camp list):
- Gold-heavy: 400+ gold in gold-cluster-only groups.
- Career: 60%+ of gems behind elk/golem/monolith, plus 200+ free gold.
- Gem-rush: 100 or less free gold, more undefended gems than defended. Our weakest class.
- Mid: groups near an altar and equally far from both spawns are worth 0.75x+ of the rest.
- Ranked record at last check: 4 picks 5-12, 3 picks 6-4, 5 picks 21-12.

**Centre-map opening expert (`opening_rounds`, off at 0; 2026-11-02):** on a map where one group holds 35%+ of the gems 20+ steps from spawn, groups past 20 steps are worth 0.1x for the first 80 rounds and are left out of the opening group. Routed on 13 maps (crater, dunes, gemfight, monolith, pilgrimage, ribs, rows, stampede, concentrated-wealth, gold-rush, starvation, famine, tangle). Against test-bot 85/140 to 91 (paired +3 -9: it rushes the centre too, so we arrive second). Live probe as v80, 1 of 10 against Fish and Mite (the win, elk_arena against Mite, is not a routed map): on gemfight our round-100 gold went from 80 to 216 (Fish) and from 0 to 705 (Mite) with fewer early deaths, and both games were still lost on the fights after round 100. The economy half works; the fight half is what is missing.

**Ranked 03homonoliths-like game the user flagged (abe8b68f, mirror roster, lost 0-1000 with 25 deaths to 5):** gold 199 to 646 at round 100 with time spent walking/acting nearly identical on both sides; they captured nine 80-gold clusters to our four, because ours spent rounds 24-42 in a fight we lost and then split to 40-gold clusters across the map while they rotated the four 80-gold clusters at the centre (respawn 50). Deaths with full bags were not the issue there: the map's only gems are two 200-gem monoliths and we never took one.

**Open leads, best first (the ones from the practice games come first; the rest are older):**
0. **Banking against teams that hold the centre.** On corners we banked nothing. Ideas: choose the altar by remembered enemy sightings, not only enemies in view (the bot forgets them at spawn and walks into the group at the centre altar); bank at a corner altar when a capture is near one; keep bags under ~40 where a tribute is at risk. `bank_nearby` and `bank_bag` alone lost against test-bot, so any rule needs the risk side too.
0. **The opening on centre maps (gemfight, vault):** Fish clears the gold camp beside spawn with everyone first and arrives at the centre as a group of five bodies with 500 gold. We walk to the centre one by one with nothing. Route by map class: a single centre capture worth 60%+ of the map's gems.
0. **Vault seat A still loses** after the standoff fix: the whole team recalls to bank 343 gems and test-bot's Prism takes the next spawns. One banker, the rest hold.
1. **`00gemfight` against naviepstokes, lost 127-1,028 (19 deaths to 0).** We led until r85 (took the 78-gem centre at r41). At r85-95 our Vendetta and HoR took Vesper to 415, then she stepped back 3 tiles, Willow healed her to 835, and our melee stood still while she shot Vendetta dead. After that our champions scatter alone to 7-gem clusters with no gold (370 gold by r800 to 4,540), and their group picks them off. Ideas: keep the group together after a lost fight, take gold-bearing camps when poor, finish or leave rather than freeze with a better rule than the failed one.
2. **The `00vault` freeze.** Seat A against `test-bot` reproduces: both teams stop acting early (ours about r32, test-bot about r180 apart from wards), 0-0 to the tiebreak over 2,000 rounds. Neither bot faults. Unexplained. The map's centre group is 49 gem clusters worth 343 gems: three captures win the game.
3. **`00corners`:** gold and deaths even, but banked gems 311 to 610 at r800. Probably a banking problem.
4. **Group movement** is what every top team has and we don't. Earlier follow attempts failed because they stopped farming, so try gathering only for fights.
5. **Hidden runners:** warding a low enemy in a bush looked good on mid maps (paired 2:0, overall 9:8). It was not adopted.
6. **The bonus for camps enemies stand on:** removing it scored 85/140 (paired 6:2). Unvalidated.

## Bot code map (`bots/baseline/src`)

- `strategy.h`: all policy constants, `TeamMemory` (the 8 shared bytes: five 11-bit claims, 9 bits free at bit 55), `DraftPlan` (1,024-byte draft memory).
- `draft/`: rosters by pick count (`team_compositions.h`) and map analysis.
- `workers/turn.cpp`: turn order (retreat, fight, bank, reinforce, farm).
- `workers/farming.cpp`: camp choice (`worker_choose_group`, SIMD lanes; some lanes are set once at allocation, so per-choice values belong in `gather`), capture holder order (`capture_worth`, `outranks`).
- `workers/skirmish.cpp`: fights (tile scoring by safety class, then kills, damage, distance), `worker_pursue`.
- `mechanics/economy.cpp`: emblem builds (fixed per champion, copied from print dont int), wards.
- `mechanics/navigation.cpp`: `move_to`, `health_recovery_needed`, `gold_value_now`.
- `workers/tribute.cpp`: banking.

Add new rules with a named constant in `policy`, so they can be switched off from an `evaluate.py` config.

**Game facts that keep mattering:** champions see only their own vision, but every champion can read the whole static map. Bushes hide units. Recall takes 5 turns and any hit cancels it. A tribute takes ceil(gems/10) turns and damage doesn't stop it. Death costs 60 rounds and hands your bag and 100 gold to the killer. Captures go to whoever holds the tile, split among allies present. The bot ends every turn in a fake recall (a state flag, not a real channel).
