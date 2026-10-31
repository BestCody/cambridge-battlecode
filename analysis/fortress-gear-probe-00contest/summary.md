# Gold-first fortress opening: 00contest probe

For the first 400 rounds on fortress-only gem maps, the candidate values fortress gems at 0.5 instead of 2.0 and applies the ordinary camp risk weight of 1.0 instead of 0.06. It then returns to the current siege values. Both policies played baseline on `00contest` in both seats at 100,000 fuel.

| Policy | Wins / 2 | Gems submitted | Gold earned | Champion deaths |
| --- | ---: | ---: | ---: | ---: |
| Current | 0 | 200 | 6,670 | 49 |
| Gold-first opening | 1 | 1,000 | 16,000 | 26 |

This local result motivated a full 140-game A/B before changing the default.

```powershell
python scripts/evaluate.py gear:fortress_gear_phase=true --bot test-bot --baseline baseline --maps 00contest --fuels 100000 --jobs 2 --out analysis/fortress-gear-probe-00contest
```
