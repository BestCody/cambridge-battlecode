# Memo: distilling "print dont int" into decision trees (abandoned)

**Status:** tried on 2026-10-28 and abandoned. The bot never played usefully, so the next step is
rule-based policies derived from PDI's games. Nothing from this attempt is in the repo any more. It's
archived locally in two git-ignored zips at the repo root. Ask Josh for them.

| archive | size | contents |
|---|---|---|
| `print_dont_int.zip` | 820 MB (4.4 GB unpacked) | 500 replays, index, extracted turns, features, all 600-round checkpoints with plots, the training code |
| `print_dont_int_final.zip` | 10 MB | the later code: compiled-tree exporter, fuel fixes, cut models, READMEs, the `bots/pdi_distill` source |

## What we did

1. **Data.** We scraped 500 games of team print dont int (PDI) and re-simulated the replays in Node
   with the cambc visualiser's engine wasm. At each PDI champion turn we queried the engine from that
   champion's perspective, so vision was enforced, and recorded what it could see and what it did.
   That gave 494 usable games and about 1.75M champion turns.
2. **Features.** Each turn became 285 features: self state, the 3 nearest visible enemies, 2 allies
   and 4 camps, remembered camp-group state, per-direction move deltas (path distance to spawn,
   altar, nearest group, best group), and teleport opportunities. We checked that the Python BFS
   matches the SDK's `distance_field`.
3. **Models.** We trained one XGBoost model per hero × decision for Vesper, Heart of Ruin,
   Marcella, Vendetta and Ryth. The decisions were move, attack target, ability, ability target,
   dash, ward, ward placement, recall, cancel, tribute, shop, teleport, teleport target and altar
   lock. The split was by game: 70% train, 10% validation, 20% test.
4. **Results.** Test accuracy for the full models, with the majority-class baseline in brackets:

   | decision | accuracy |
   |---|---|
   | move | 0.80–0.82 (0.24–0.35) |
   | attack | 0.95–0.98 (0.72–0.79) |
   | ability | 0.94–0.98 |
   | recall, cancel | 0.99 |
   | shop | 0.99 |
   | dash | 0.62–0.65 |
   | ward placement | 0.46 |
   | teleport, lock | no better than "never" |

5. **Bot.** `bots/pdi_distill` ran a C++ port of the features plus the exported trees. Against
   baseline on 00anvil it scored **0 gold and 0 gems**. Vesper walked a few tiles, then picked
   "stay" while cancelling and restarting a recall every turn for the rest of the game.

## What we learned

- **Fuel is dominated by SDK host calls.** Every `bitboard::is_set` and `distance_field::get` is a
  host call. The feature code first cost about 70k fuel per turn; exporting the map once into plain
  arrays brought it to about 38k.
- **Trees are expensive.** Walking node arrays cost about 45 fuel per tree level. Compiling the
  trees to C++ conditionals brought that to about 9. Even so, the full 600-round models would cost
  about 1M fuel per turn, so we shipped only the first 10–30 boosting rounds. Move accuracy fell
  from 0.81 to 0.72.
- **Source size matters.** Compiled trees came to about 5 MB of source against the 5 MB submission
  limit. Ward placement alone was about 100 KB per hero, for an unimportant decision.
- **Imitation copies habits, not goals.** Most PDI turns are "stay" or a speculative recall, and the
  trees reproduced that without the reasons behind it. The C++ feature port was also never verified
  against the Python features in a live game, so a mismatch may have made it worse.
- **Small, sensible decisions.** Several decisions (recall, cancel, tribute, shop) have small trees
  and very high accuracy. That suggests a few simple rules explain them.

## What PDI does (from the macro report)

- **Record:** 77% wins over the 500 games. All losses were on gems, none by timeout. Their weak maps
  were 00expanse (0–10) and 01gold_rush (0–4).
- **Draft:** a fixed order by team size.
  - 3: Vesper > Marcella > HoR
  - 4: Vesper > HoR > Vendetta > Marcella
  - 5: Vesper > HoR > Vendetta > Ryth > Marcella
- **Speculative recall:** they start a recall at the end of about 83% of turns and cancel it next
  turn about 70% of the time, so committing to go home is just *not cancelling*. Real trips home:
  about 11–14 per game, at 40–70% health. Tributes: about 5–10 per game.
- **Time split:** about 45–58% of turns walking, 17–27% farming (mostly wolves and elks), 4–6%
  fighting champions, 5–9% dead.
- **Builds:** they open with a Treasure shard, then buy an altar lock. The main lines are Bruiser
  for Vesper and Marcella, Treasure/Power for HoR, Treasure str for Vendetta, and Durability hp
  throughout. They buy teleport crystals and locks often and use them rarely (about one teleport
  per hero per game).

## Next

Derive explicit rules from the extracted turns: unzip only `training/data/raw/`, then ask targeted
questions with short scripts or depth-2/3 trees. Topics: opening route, how they choose the next
camp group, when they genuinely recall, gem threshold before tributing, when they fight or back
off, and shop order. Then implement those rules on top of `bots/baseline`.
