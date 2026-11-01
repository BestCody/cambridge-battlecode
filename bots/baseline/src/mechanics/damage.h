#pragma once
// The damage model: the per-unit building blocks every damage, reach and
// threat estimate is made of (included by bot.h after ObservedUnit). Each
// estimate keeps its own assumptions as arguments (counts a stun or not, a
// dash or not, a horizon, a margin); the tile-level sums (incoming(),
// incoming_scan(), exit_damage(), sides_at(), the micro's sides, ...)
// combine these, and the units' terms are read once per sighting
// (ObservedUnit).
//
// Inline, and each takes its unit through Ref (a plain pointer): a
// reference parameter lets the compiler evaluate both sides of a `&&` or a
// `?:` (it may read any field of a referenced unit), which cost more fuel
// than the short-circuit tests as written where these are used.
namespace damage {

template <class T>
struct Ref {
    const T *p;
    Ref(const T &t) : p(&t) {}
    const T *operator->() const { return p; }
    const T &operator*() const { return *p; }
};
using Unit = Ref<ObservedUnit>;

// ---- Who acts ----

// A stun with time left holds the unit through its next action; one whose
// timer has run out ends before it acts.
inline bool held(Unit u) { return u->stunned && u->stun_time > 0; }

// A spell that strikes what it reaches: a damage, control or area spell, or
// a damaging dash (Heart of Ruin's lands its area its steps plus its radius
// away). A control spell counts with no damage of its own.
inline bool damaging(Ref<Spell> s) {
    return s->kind == bc::ability_kind::damage || s->kind == bc::ability_kind::cc ||
           s->kind == bc::ability_kind::aoe_damage || (s->kind == bc::ability_kind::dash && s->modifier > 0);
}
// A spell's reach from where its caster stands: its range and its radius.
inline int cast_reach(Ref<Spell> s) { return int(s->range + s->radius); }
// The longest reach of a kit's damaging spells, from `far` (an attack range,
// or 0): what a unit of the kit can strike from where it stands.
inline int kit_reach(const std::array<Spell, 2> &spells, int far) {
    for (const auto &s : spells)
        if (damaging(s))
            far = std::max(far, cast_reach(s));
    return far;
}
// Heart of Ruin's passive on a unit's spells as on its hits (the strength
// reading already has it; a spell burst made from focus does not).
inline float spell_bonus(Unit e) {
    return e->type == bc::champion_type::heart_of_ruin ? hor_bonus(e->health, e->max_health) : 1.0f;
}

// ---- Reach ----

// Where a unit strikes from without moving: its attack or any damaging
// spell, ready or not (ObservedUnit::spell_reach).
inline const int &standing_reach(Unit u) { return std::max(u->range, u->spell_reach); }
// Its move before its next action: none while a stun holds it when
// `stun_holds` (a caller that ignores stuns passes false).
inline int move_by_next(Unit u, bool stun_holds) { return stun_holds && held(u) ? 0 : u->move_speed; }
// Where it can act by its next turn: its standing reach and a move, plus a
// margin of tiles.
inline int act_reach(Unit u, bool stun_holds, int margin = 0) {
    const int move = move_by_next(u, stun_holds);
    return standing_reach(u) + move + margin;
}
// Tiles a unit closes on us in a round and still strikes: a melee one walks
// and strikes in one turn, a ranged one cannot walk and shoot unless a ready
// attack reset gives the shot back; a ready dash adds its tiles (in place of
// the walk when no walk can follow it). One rule for the chase, the threat
// bits and the exit lanes.
// (By reference, as these two have always been: their callers' code is
// made for that.)
inline int closing_tiles(const ObservedUnit &e, bool reset, bool dash) {
    const int walk = e.range <= 1 || reset ? e.move_speed : 0;
    if (!dash || e.dash_range <= 0)
        return walk;
    return e.dash_walks ? walk + e.dash_range : std::max(walk, int(e.dash_range));
}
// The reach of its basic strike next round, closing in (closing_tiles()).
inline int strike_reach(const ObservedUnit &e, bool reset, bool dash) { return e.range + closing_tiles(e, reset, dash); }

// ---- Damage next turn (incoming()) ----

// An enemy strikes before our next turn unless dead, held through its turn
// or teleporting (incoming()). Its terms next turn, one function a term (a
// caller packing them reads each where it writes it): its hit reaches
// `range`, its ready burst `spell_reach` (cast from where it stands); the
// reach of a step and a hit (a melee one, or a ranged one with a ready
// reset; -1 none); whether it is melee (-1) or not (0); a ready reset's
// second hit. Its hit is `strength`, its ready burst `spell_burst` (Heart of
// Ruin's passive included in both). The step closes as closing_tiles()
// counts it, with a ready dash (Heart of Ruin, Vendetta and Vesper dash 2
// tiles and strike; without it a tile 3 from a melee one read as safe).
inline int lunge_reach(Unit e) {
    const bool dash = e->dash_wait == 0 && e->dash_range > 0;
    return e->range <= 1 || e->reset_ready || dash ? damage::strike_reach(*e, e->reset_ready, dash) : -1;
}
inline int melee_lane(Unit e) { return e->range <= 1 ? -1 : 0; }
inline float reset_hit(Unit e) { return e->reset_ready ? e->strength : 0.0f; }
// Its blow on a unit it reaches with everything: one ability a turn, so its
// hit, then a ready reset's second hit or its ready burst, whichever is more.
inline float blow(Unit e) { return e->strength + std::max(reset_hit(e), e->spell_burst); }

// ---- Damage while we leave (exit_damage()) ----

// Its terms for a unit walking out of its reach a tile a round: the rounds a
// stun holds it, and the reach of its hits while we leave (a walk and a hit,
// or its spell reach). exit_packed() adds, from its kit's ready slots, its
// first-round strike (strike_reach(), its reset and dash as read), each
// slot's burst (a reset's hit, or a damaging() spell's focus x modifier x
// spell_bonus()) and its reach (cast_reach(), plus a walk unless the spell
// excludes one), and a ready stun's reach; an unseen unit's slots and dash
// count as ready. Its hit is strength.
inline int exit_held(Unit e) { return held(e) ? e->stun_time : 0; }
inline int exit_reach(Unit e) { return std::max(e->range + e->move_speed, e->spell_reach); }

// ---- Camps ----

// A camp strikes: it has strength and no stun holds it.
inline bool camp_strikes(Unit c) { return c->strength > 0 && !held(c); }
// The rounds a unit `d` tiles from a striker of reach `reach` takes hits as
// it walks out a tile a round (the striker acts first each round).
inline int rounds_in_reach(int reach, int d) { return reach - d + 1; }
// A hit shared among the `ties` other units as near the striker.
inline float shared_hit(float hit, int ties) { return hit / float(1 + ties); }

// ---- Power ----

// Its ready burst (ObservedUnit::burst), none while a stun holds it when
// `stun_holds`.
inline float ready_burst(Unit u, bool stun_holds) { return stun_holds && held(u) ? 0.0f : u->burst; }
// A unit's fighting power (unit_power()) with `health` as its health: its
// shields on top, its ready_burst().
inline float power(Unit u, float health, bool stun_holds) {
    return unit_power(health + u->shield + u->ready_shield, u->damage_rate, ready_burst(u, stun_holds),
                      u->healing_rate, u->mitigation);
}

} // namespace damage
