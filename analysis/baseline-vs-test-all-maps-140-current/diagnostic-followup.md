# Follow-up on maps with little or no banking

These figures come from the uninstrumented 100,000-fuel, two-seat control run. They describe the failure without assuming the tribute rule alone caused it.

| Map | Test-bot gems submitted | Baseline gems submitted | Test-bot gold earned | Baseline gold earned | Test-bot deaths | Baseline deaths |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `02guardians` | 0 | 2,258 | 6,185 | 15,409 | 36 | 27 |
| `01boss-battle`, scoreless test-bot seat | 0 | 1,045 | 13,635 | 23,746 | 12 | 8 |

On `02guardians`, test-bot collected 1,901 gems in the two games but banked none. Its large gold deficit and extra deaths indicate a wider failure to control the central area; lowering a bag threshold alone is unlikely to solve it. The baseline-roster A/B still lost both seats, and the narrow Prism banking and early carrier banking A/Bs still produced zero submissions there.

On the scoreless `01boss-battle` seat, test-bot ended with 1,151 carried gems, including 657 on Marcella. A local early carrier bank changed that match to a win: Marcella submitted 411 gems at round 921 and 1,049 at round 1269. Across all maps, however, that rule lost one net match, so it remains disabled.

Verbose bot stdout changed the 100,000-fuel `02guardians` match by exhausting fuel on many turns. Those traces are not representative and were discarded. The uninstrumented match summaries do not identify whether tribute was declined because of a route, lock, danger, or skirmish preemption. A future trace should use compact state recording with a measured fuel budget or a replay decoder before changing altar routing globally.
