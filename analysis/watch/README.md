# Watching fights

Rebuild fights from replays step by step (every move, attack, ability, stun
and camp hit) and draw them as filmstrips, using the visualiser's own engine
module. This is the default way to analyse games: watch the fights first,
then decide what to change.

Engine module: the visualiser's copy in the cambc assets, for the game's
engine version, e.g.
`~/AppData/Local/cambc/assets/<hash>/visualiser/assets/v1_0_3-*.wasm`.

1. Download a match: `cambc matches download <id> -o m.zip` and
   `cambc matches summary <id> --json -o m.json` (match groups list their
   match ids).
2. One line per round for picking fights:
   `node replay_dump.mjs <engine.wasm> m.zip m.jsonl`. A jobs file has one
   game per line: `m.jsonl <our team 0/1> <map> m.json <opponent>`.
3. Pick fights: `python -I -X utf8 select_fights.py jobs.txt [maxround]`
   lists our failed chases (A), deaths in fights we outnumbered at contact
   (B), retreats that ended in death (C), and the opponent's successful ones
   (A+, B+, C+), with the focus champion and the rounds to watch.
4. Record a window step by step:
   `node watch_fight.mjs <engine.wasm> m.zip <from> <to> f.json`.
5. Draw and log:
   `python -I -X utf8 render_fight.py f.json <our team> <focus id> <first> <last> f.png [columns] > f.txt`.
   Look at `f.png` (legend in the script's docstring) and read `f.txt`: per
   round each champion's health and tile, then each step (attacks with
   health before and after, abilities, stuns, recalls, camp hits, blocked
   moves), and at the end damage taken from champions and from camps,
   rounds ending stunned, and who killed whom.

Notes: vision is per champion, so "unseen" means none of our champions saw it.
Awake camps attack the nearest champion every turn.
