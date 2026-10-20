# Squad design notes

This document specifies the Stellar/Tavi/Cassi squad controller in `bots/newest-bot/src/squad/`. Each rule below names the code that implements it.

- **Done** means the rule is implemented.
- **Partial** means a simpler version of the rule is implemented, and the gap is described.
- **Open** means the rule is not implemented.

None of these rules have been match-tested yet. Validate them with `scripts/ladder.ps1` before tuning the `policy::` constants they introduce.

Terms used throughout:

- **Reward time**: rounds until the camp's reward is captured (travel + clear + occupancy + capture timer).
- **Interference**: the earliest round at which an enemy champion could stand on the capture tiles.
- **Material event**: a member's death, a new relevant enemy, a lost capture timer, or a terrain/obstacle change.

## 1. Central contest selection

**1.1 Rank by contest first, then by gems.** The coordinator groups jobs into three categories and takes the best-rated job from the first non-empty one:

1. Contested gem camps: the enemy's terrain distance is at most our home distance + 2 × the map's camp spacing.
2. Other gem camps.
3. Gold-only camps.

The current job keeps `commit_bonus` only against jobs in the same category. *Done*: `select_squad_job`, [selection.cpp](../bots/newest-bot/src/squad/selection.cpp).

**1.2 Leave home camps to workers.** A camp that a living worker (Issac, Ryth or Varg) currently claims is skipped during ranking. The exceptions are a contested camp and the squad's own current camp. Gold camps reach the squad only as a preparation detour (1.4) or when no gem job exists. *Done*: ranking loop in `select_squad_job`.

**1.3 Price interference from enemies we cannot see.** A hidden enemy that (re)spawned at round `s` can be no closer to a camp than `enemy_distance - (round - s)`. While that bound is positive, the overlap with our reward time is added to the job's interference work, scaled by `policy::hidden_interference_weight` (0.5). Once the bound reaches zero the enemy could be anywhere, so it stops contributing; counting it would only tax every job equally. Visible enemies are still timed from their real positions. *Done*: `hidden_interference` in [coordination.cpp](../bots/newest-bot/src/squad/coordination.cpp), and stages 2–3 of `estimate_job` in [objectives.cpp](../bots/newest-bot/src/squad/objectives.cpp).

**1.4 Detour home only if it pays for itself.** A gold or upgrade detour replaces a contested siege job only when the clear time it saves exceeds the detour (reward + recall + walk back) × `commit_bonus`. *Done*: existing preparation block in `select_squad_job`.

**1.5 Clear immediately when uncontested.** If the plan's interference is later than its reward time, and no new relevant enemy has appeared since the plan was made, the squad clears without waiting for every member to reach its firing tile. Otherwise, while a relevant enemy is visible, members hold their cooldowns for denial until the squad is in position. *Done*: clear gate in `squad_resource_turn`, [stellar_tavi_cassi.cpp](../bots/newest-bot/src/squad/stellar_tavi_cassi.cpp).

**1.6 Fall back to other forward camps first.** If the selected contest is rejected, the comparison falls through to the next category before any preparation work. *Done*: follows from 1.1.

## 2. Planning and action scheduling

**2.1 Resume searches without losing work.** Ranking is cheap and covers all camps. Detailed estimates run only for `objective_finalists` (6) plus the current job. Each search keeps its cursor and best result across turns, and running out of fuel never clears a valid job. *Done*: unchanged.

**2.2 Invalidate plans on material change, not age.** A plan records the terrain revision, camp status and relevant-threat count it was made under. It is re-estimated when any of these change, or when membership changes. Age alone expires a plan only after `policy::plan_lifetime_windows` (3) work windows; it used to be 1. Calibration after each clear window still forces a refresh. *Done*: `prepare_objective` in objectives.cpp, and the new `JobEstimate` fields in [squad.h](../bots/newest-bot/src/squad/squad.h).

**2.3 Selection comes before optional work.** With no job, the coordinator runs selection before optional combat searches or ward placement, keeping `planning_reserve` back for a legal action and publication. Route caches only use leftover fuel. *Done*: unchanged ordering in `play_squad_turn`.

**2.4 Resume deferred moves first.** If last turn's move was deferred for fuel, the next turn moves before running a new estimate. The estimate still runs at the end of the turn on leftover fuel. *Done*: `resume_motion` in `squad_resource_turn`.

**2.5 Publish actual results.** `squad.motion` records moved, arrived, waiting, blocked or deferred, with a reason. Progress is credited only for real improvements (4.3). *Done*.

## 3. Combat follow-through

**3.1 Cassi controls a secondary threat.** The shared focus target is marked. A stun on the focus target earns no control credit if the allies who can still act this round will finish it anyway. The exception is a channelling target, where the stun is an interruption. A stun on a *different* enemy that threatens a member gets × `policy::secondary_control_bonus` (1.25). When control adds nothing, the combo search's best option is Cassi's damage spell. *Done*: `attack_combo` in [micro.cpp](../bots/newest-bot/src/mechanics/micro.cpp).

**3.2 Walk-stun needs a concrete payoff.** A stun cast after stepping (Cassi's walk-compatible slot) earns control credit only if one of these is true:

- the target is channelling;
- the target can kill an ally or this champion next turn;
- an ally who has not yet finished its turn can follow up on the target.

Allies who have already finished the round are excluded from that last check, because an enemy turn may come before they act again. *Done*: `walked_for_it` in `attack_combo`.

**3.3 No control without a useful window.** `control_value` is zero unless the enemy can reach a member within its own movement. Reach now uses the enemy's `move_speed`, not ours. Stun expiry, immunity and health are re-read every turn, so there is no fixed stun rotation. *Done*: [coordination.cpp](../bots/newest-bot/src/squad/coordination.cpp).

**3.4 Order turns by tier.** Next-round priority uses disjoint tiers from `priority_tier` in [strategy.h](../bots/newest-bot/src/strategy.h):

| Tier | Base | Condition |
| --- | --- | --- |
| finish | 4000 | A lethal hit, a stun on a channelling enemy, or damage on a recalling enemy |
| control | 3000 | A stun that a visible member can follow up with damage |
| damage | 2000 | Any other damage on a relevant enemy |
| idle | 1000 | No opportunity |

Value within a tier is capped at 999. Workers use the same tiers, so a worker's raw damage can no longer outrank a squad finishing blow. *Done*: `squad_priority` and `worker_priority`.

**3.5 Keep shots in place.** Ranged members that attacked cannot walk in the same turn, so members with productive shots stay put. A threatened member retreats through `health_recovery_needed`. *Partial*: there is no explicit "hold position while a teammate repositions" coordination.

**3.6 Stop pursuit when it stops mattering.** `relevant_enemy` limits combat to enemies near a member, the job's camp or the delivery altar. A sole capture holder never steps off the capture tiles to chase. *Done*: unchanged.

## 4. Continuing or abandoning a long clear

**4.1 Check reward access before committing.** Before clearing, the estimate assigns capture tiles, times the slowest member's occupancy, and simulates each visible contender against our capture-window actions. *Done*: unchanged stages 1–3 of `estimate_job`.

**4.2 Re-plan on material events.** The coordinator detects a member death, an increase in relevant threats, and a lost capture timer. Each one sets the job for review this round and invalidates the plan. Terrain obstruction is handled through 2.2. *Done*: `detect_material_events` in coordination.cpp.

**4.3 Count only real progress.** Progress means one of:

- a new best route distance for the current goals;
- our own damage on the camp;
- a held capture;
- a bank/crossing stage advancing.

Stepping away and back no longer counts, because only a new best distance does. Camp health lost while an enemy is in range of the camp is not counted, because their damage gives us no control. Reselecting the same camp never resets the deadline. *Done*: `squad_move` in [movement.cpp](../bots/newest-bot/src/squad/movement.cpp) and `observe` in [world.cpp](../bots/newest-bot/src/mechanics/world.cpp).

**4.4 Treat damage dealt as sunk cost.** Estimates use remaining health only. The clear gate no longer treats a partly damaged camp as a reason to keep clearing under threat (see 1.5). *Done*.

**4.5 Repeated capture resets.** After `policy::capture_reset_limit` (2) lost capture timers on the same job, the job loses its commitment bonus. It must then beat alternatives on its own estimate. *Done*: `select_squad_job`.

**4.6 Cooldown waits.** A wait for cooldowns counts as progress once the next ready action lands. *Open*: ranged basic attacks are available every turn, so this has not been needed.

## New policy constants

All live in `policy::` in [strategy.h](../bots/newest-bot/src/strategy.h) so `scripts/benchmark.ps1` can grid-search them.

| Constant | Default | Disables at |
| --- | --- | --- |
| `plan_lifetime_windows` | 3 | 1 (old age-only refresh) |
| `hidden_interference_weight` | 0.5 | 0 |
| `capture_reset_limit` | 2 | a large value |
| `secondary_control_bonus` | 1.25 | 1.0 |

## Archived complete rosters

These eight alternatives are kept for reference; they are not an instruction to re-enable roster selection. The live rosters are in [team_compositions.h](../bots/newest-bot/src/draft/team_compositions.h). Twins is one draft pick that produces Cassi and Issac.

| Picks | Complete roster | Intended use |
| --- | --- | --- |
| 3 | Stellar + Tavi + Twins | Ranged burst and control, with Issac available for separate resource work |
| 3 | Stellar + Tavi + Ren | Concentrated aggression against opponents approaching the squad |
| 3 | Stellar + Ren + Twins | Control and burst |
| 3 | Varg + Vendetta + Twins | Spread-out gathering and rapid camp clearing |
| 4 | Ren + Ryth + Stellar + Twins | Additional control and melee clearing |
| 5 | Varg + Ryth + Stellar + Tavi + Twins | Ranged pressure alongside strong resource clearing |
| 5 | Ren + Ryth + Stellar + Varg + Twins | Broader control and melee presence |
| 5 | Prism + Ryth + Stellar + Tavi + Twins | Concentration on exceptionally valuable gem camps |
