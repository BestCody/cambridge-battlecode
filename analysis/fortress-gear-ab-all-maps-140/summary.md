# Gold-first fortress opening A/B: all maps

The current test-bot and a candidate that farms safer gold camps for the first 400 rounds on fortress-only gem maps each played baseline on all 70 maps in both seats at 100,000 fuel. The candidate uses a 0.5 gem reward multiplier and ordinary 1.0 risk weight during that opening, then reverts to the existing 2.0 gem multiplier and 0.06 risk weight. Seeds and map hashes matched by map and test-bot seat.

| Test-bot policy | Wins / 140 | Gems collected | Gems submitted | Gold earned | Champion deaths | Bot faults |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Previous default | 68 | 132,469 | 102,417 | 691,133 | 836 | 0 |
| Gold-first fortress opening | 69 | 133,269 | 103,217 | 700,463 | 813 | 0 |

One paired winner changed: `00contest` with test-bot in seat B. It changed from a 0-gem loss to an 800-gem win. The other seat still lost but submitted 200 gems as before. Across those two matches, test-bot gold earned rose from 6,670 to 16,000 and deaths fell from 49 to 26. No other map-seat pair changed its test-bot submitted score in this A/B. The fortress opening is enabled by default.

The 69/140 result is a deterministic one-game-per-map-and-seat comparison, not an estimate from independent random games. A later run on a separate map or seed set would test generalization.

```powershell
python scripts/evaluate.py gear:fortress_gear_phase=true --bot test-bot --baseline baseline --fuels 100000 --jobs 4 --out analysis/fortress-gear-ab-all-maps-140
```
