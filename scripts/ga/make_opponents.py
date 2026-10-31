"""Builds the GA's extreme-style opponents from bots/baseline/src, each one
behaviour pushed to its limit, into scripts/ga/opponents/opp_<style>/src:

  rusher    always attacks: the nearest enemy champion in view, else walks to
            the enemy's side; never farms or retreats (only a winning bag banks)
  farmer    never fights: no skirmish, pursuit or reinforcing; farms, banks
            and still runs from danger
  turtle    farms its own half: enemy-side camps nearly worthless, long walks
            heavily penalised
  thief     prefers the enemy's side: enemy-side camps worth three times ours
  deathball moves as one: the cohesion penalty very high on every map

They are test fixtures for the GA only (they are not our bot). Run again after
baseline changes: python scripts/ga/make_opponents.py"""
import os, re, shutil, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SRC = os.path.join(REPO, "bots", "baseline", "src")
OUT = os.path.join(REPO, "scripts", "ga", "opponents")


def edit(path, pairs):
    s = open(path, "rb").read().decode("utf-8")
    nl = "\r\n" if "\r\n" in s else "\n"
    for a, b in pairs:
        a, b = a.replace("\n", nl), b.replace("\n", nl)
        if s.count(a) != 1:
            sys.exit(f"{path}: anchor found {s.count(a)} times:\n{a}")
        s = s.replace(a, b, 1)
    open(path, "wb").write(s.encode("utf-8"))


def constants(src, values):
    path = os.path.join(src, "strategy.h")
    s = open(path, "rb").read().decode("utf-8")
    for name, value in values.items():
        pattern = re.compile(r"(inline constexpr [\w:]+ " + re.escape(name) + r" = )([^;]+)(;)")
        if len(pattern.findall(s)) != 1:
            sys.exit(f"{name}: not exactly one definition")
        s = pattern.sub(lambda m: m.group(1) + value + m.group(3), s)
    open(path, "wb").write(s.encode("utf-8"))


RUSH = '''    // GA opponent "rusher": the nearest enemy champion in view is attacked;
    // with none in view, the walk goes to the enemy's side. No farming.
    {
        const ObservedUnit *prey = nullptr;
        for (const auto &e : enemies)
            if (e.health > 0 && !enemy_spawn_tile[tile_index(e.position)] &&
                (!prey || distance(position, e.position) < distance(position, prey->position)))
                prey = &e;
        if (prey) {
            if (distance(position, prey->position) > range)
                move_to(around(prey->position, float(range)) & open_terrain, prey->position, true, true);
            combat(true);
        } else {
            const vec2 far_side = enemy_spawn_tiles.empty() ? position : enemy_spawn_tiles[0];
            move_to(around(far_side, 6.0f) & open_terrain, far_side, false, true);
            combat(false);
        }
        note_activity(Activity::fight);
        return;
    }
    // A champion without a camp chooses one before anything else this turn.'''


def make(name, apply):
    dst = os.path.join(OUT, f"opp_{name}")
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(SRC, os.path.join(dst, "src"), ignore=shutil.ignore_patterns(".cambc"))
    apply(os.path.join(dst, "src"))
    print("made", dst)


def rusher(src):
    edit(os.path.join(src, "workers", "turn.cpp"), [
        ('''    retreating |= health_recovery_needed();
    if (retreating && worker_escape()) {''', '''    retreating = false; // The rusher never retreats.
    if (retreating && worker_escape()) {'''),
        ('''    // A champion without a camp chooses one before anything else this turn.''', RUSH),
    ])


def farmer(src):
    edit(os.path.join(src, "workers", "turn.cpp"), [
        ('''    worker_update_formation();
    if (worker_skirmish() || worker_pursue()) {''', '''    // GA opponent "farmer": never reinforces, skirmishes or pursues.
    if (false) {'''),
    ])
    edit(os.path.join(src, "workers", "tribute.cpp"), [
        ('''if (skirmish_declined != int(round) && worker_skirmish())''',
         '''if (false) // The farmer never skirmishes.'''),
    ])


def turtle(src):
    constants(src, {"enemy_side_factor": "0.02f", "travel_hazard": "0.4f"})


def thief(src):
    constants(src, {"enemy_side_factor": "3.0f"})


def deathball(src):
    constants(src, {"cohesion_hazard": "0.5f", "cohesion_max_spacing": "1000.0f"})


if __name__ == "__main__":
    for name, apply in [("rusher", rusher), ("farmer", farmer), ("turtle", turtle), ("thief", thief),
                        ("deathball", deathball)]:
        make(name, apply)
