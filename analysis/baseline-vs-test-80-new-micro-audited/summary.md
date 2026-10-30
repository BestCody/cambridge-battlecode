# Baseline vs audited test-bot: 80 games

Ran `scripts/evaluate.py` with `--bot baseline --baseline test-bot`, the same eight maps, both seats, and fuel budgets 92,000, 96,000, 100,000, 104,000, and 108,000 as the previous micro evaluation. The `test-bot` working tree includes the audit fixes for Willow healing, skirmish retreat, turn state, and duplicate work.

| Metric | Baseline | Test-bot |
| --- | ---: | ---: |
| Wins | 33 | 47 |
| Submitted gems | 56,150 | 56,461 |
| Bot faults | 0 | 0 |

Baseline won 20 of 40 games in seat A and 13 of 40 in seat B. The previous run, before the audit fixes, was 61–19 for baseline; this run changed 28 wins toward `test-bot`. The combined run does not isolate the effect of any one fix.

| Map | Baseline wins / 10 |
| --- | ---: |
| 00ambush | 2 |
| 00collision | 5 |
| 00contest | 10 |
| 00corners | 1 |
| 00eight | 6 |
| 00lost-world | 1 |
| 00swirl | 7 |
| 01big-world | 1 |

All 80 statistics files and replays are in this directory, along with `games.json`.
