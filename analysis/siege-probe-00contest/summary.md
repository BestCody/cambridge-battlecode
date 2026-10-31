# Fortress-target probe on 00contest

The map's score gems come from two fortress camps, while its many small camps pay gold but no gems. The current test-bot submitted 200 gems across both seats and lost both games. A local probe raised `siege_gem_weight` from 2 to 8 and lowered `siege_risk_weight` from 0.06 to 0.01 to prioritize fortress targets.

| Policy | Wins / 2 | Gems submitted | Gold earned | Champion deaths |
| --- | ---: | ---: | ---: | ---: |
| Current | 0 | 200 | 6,670 | 49 |
| Stronger siege targeting | 0 | 0 | 1,260 | 61 |

The stronger target incentive sent the team into more deaths and reduced its economy. It is not enabled. The replay summaries show baseline's first 200-gem fortress collection at round 649 versus test-bot's at round 800 in the seat where test-bot collected any score gems. A viable change needs to improve the party's survival and damage on the fortress, not merely increase its target score.

```powershell
python scripts/evaluate.py siege:siege_gem_weight=8.0f,siege_risk_weight=0.01f,fortress_gear_phase=false --bot test-bot --baseline baseline --maps 00contest --fuels 100000 --jobs 2 --out analysis/siege-probe-00contest
```
