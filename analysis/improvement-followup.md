# Test-bot improvement follow-up

The common control was the previous test-bot policy against baseline on all 70 maps in both seats at 100,000 fuel: **68/140 wins**. Candidate matches used the same map hash, seed, and test-bot seat. Each candidate changed the named policy while the gold-first fortress opening was still off.

| Candidate | Wins / 140 | Change vs control | Decision |
| --- | ---: | ---: | --- |
| Baseline's draft roster | 47 | -21 | Keep current roster |
| Narrow Prism endgame banking | 68 | 0 | Keep fixed Prism banking |
| Farm-value weight package | 63 | -5 | Keep current weights |
| Early safe-carrier banking | 67 | -1 | Keep current turn order |
| Gold-first fortress opening | **69** | **+1** | **Enabled** |

The enabled rule applies only when the draft finds no cheap gem-bearing camp. For the first 400 rounds it prioritizes safer gold farming, then returns to the existing fortress siege weights. It changed one paired winner: `00contest` with test-bot in seat B went from a 0-gem loss to an 800-gem win. Across all 140 matches, test-bot gold earned rose from 691,133 to 700,463, gems submitted from 102,417 to 103,217, and deaths fell from 836 to 813. All other map-seat submitted scores matched the control.

The experiments also narrowed two unresolved cases. On `02guardians`, test-bot banked no gems in either seat, had 36 deaths against baseline's 27, and earned 6,185 gold against 15,409. Its central-area economy and survival remain the main problem; none of the tested banking or draft changes fixed it. On the scoreless `01boss-battle` seat, early carrier banking won that one match but lost a net match across all maps. A stronger fortress target score on `00contest` caused more deaths and no gems; the safer gold-first opening worked better.

Detailed results: [roster](roster-ab-all-maps-140/summary.md), [Prism banking](banking-ab-all-maps-140/summary.md), [farm weights](farming-ab-all-maps-140/summary.md), [carrier banking](carrier-ab-all-maps-140/summary.md), [gold-first fortress opening](fortress-gear-ab-all-maps-140/summary.md), and [no-deposit diagnostics](baseline-vs-test-all-maps-140-current/diagnostic-followup.md).

These are deterministic games on one map pool and seed schedule. The +1 result establishes improvement on that schedule, not a general win-rate estimate.
