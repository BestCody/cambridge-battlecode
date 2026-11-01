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

**State:** `baseline` commit 07ebd07 is v78, live, in follow-newest mode at last check. It wins about 81/140 against `test-bot`. The target the user wants is 100+/140.

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

**What matters most in `baseline`** (ablation): recall home after a capture (57/140 without it), the long-plan penalty (64), retreating at low health (67), capture holder order, and Prospector and Durability emblems.

**Map classes** (cheap in the draft, from the camp list):
- Gold-heavy: 400+ gold in gold-cluster-only groups.
- Career: 60%+ of gems behind elk/golem/monolith, plus 200+ free gold.
- Gem-rush: 100 or less free gold, more undefended gems than defended. Our weakest class.
- Mid: groups near an altar and equally far from both spawns are worth 0.75x+ of the rest.
- Ranked record at last check: 4 picks 5-12, 3 picks 6-4, 5 picks 21-12.

**Open leads, best first:**
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
