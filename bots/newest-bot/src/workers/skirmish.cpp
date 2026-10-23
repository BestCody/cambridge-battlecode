// Worker skirmish decisions against enemy champions.
#include "../bot.h"

bool Bot::worker_skirmish() {
    profiling::Scope scope(profiling::Stage::skirmish);
    if (enemies.empty())
        return false;
    const auto abilities = bc::self().abilities();
    // Nothing acts before the second use below, so readiness is read once.
    std::array<bool, 2> is_ready{};
    {
        const bool can_cast = bc::self().can_cast();
        for (unsigned i = 0; i < is_ready.size(); ++i)
            is_ready[i] = ready(abilities[i], can_cast);
    }
    int ready_reach = range;
    for (unsigned i = 0; i < spells.size(); ++i)
        if (is_ready[i] && spells[i].modifier > 0 &&
            (spells[i].kind == bc::ability_kind::damage || spells[i].kind == bc::ability_kind::cc ||
             spells[i].kind == bc::ability_kind::aoe_damage))
            ready_reach = std::max(ready_reach, int(spells[i].range + spells[i].radius));
    int selected = -1;
    float best = -1;
    bool selected_contact = false;
    for (std::size_t i = 0; i < enemies.size(); ++i) {
        const auto &e = enemies[i];
        if (enemy_spawn_tile[tile_index(e.position)])
            continue;
        int d = distance(position, e.position);
        // Choose the current engagement among enemies we can actually damage.
        // A wounded runner outside our reach must not redirect the whole fight
        // while other enemies are already inside our firing line.
        const bool contact = d <= ready_reach || (range <= 1 && d <= range + int(stats.base_move_speed));
        if (selected_contact && !contact)
            continue;
        float value = (150 + e.gems * 5 + e.strength * 2) / (std::max(100.0f, e.health + e.shield) * (d + 2));
        if (e.channel)
            value *= 2;
        if (value > best || (contact && !selected_contact)) {
            best = value;
            selected = i;
            selected_contact = contact;
        }
    }
    if (selected < 0)
        return false;
    const auto victim = enemies[selected];
    float ours = friendly_power(victim.position), theirs = enemy_power(victim.position);
    float danger = incoming(position);
    // Price in imminent enemy respawns: a fight beside their base can be joined
    // by a full-health body, so the effective enemy power must reflect it.
    theirs += respawn_threat(position);
    float spell_burst = 0;
    for (unsigned i = 0; i < 2; ++i)
        if (is_ready[i]) {
            if (spells[i].kind == bc::ability_kind::attack_reset &&
                distance(position, victim.position) <= range)
                spell_burst = std::max(spell_burst, strength);
            else if ((spells[i].kind == bc::ability_kind::damage || spells[i].kind == bc::ability_kind::cc ||
                      spells[i].kind == bc::ability_kind::aoe_damage) &&
                     distance(position, victim.position) <= spells[i].range + spells[i].radius)
                spell_burst = std::max(spell_burst, focus * spells[i].modifier);
        }
    float burst = spell_burst + (distance(position, victim.position) <= range ? strength : 0);
    bool kill = victim.health + victim.shield <= burst * victim.mitigation;
    // A sliver of enemy health is a kill we can secure next turn. Finishing
    // removes their damage and any carried gems, so do not recall out of a
    // winning duel because the one-turn health math looks unfavourable. Only
    // chase a *channeling* sliver (recalling/depositing): interrupting it is
    // high-value and it is not fighting back, whereas a sliver still trading
    // has teammates behind it that punish an over-extend.
    bool nearly_dead = victim.health + victim.shield <= victim.max_health * 0.2f;
    bool finish = nearly_dead && victim.channel && health > danger * 1.5f;
    bool favourable = ours * policy::aggression >= theirs;
    if ((health < danger * 2.2f || !favourable) && !kill && !finish) {
        // Break contact with a stronger group that can soon reach us rather
        // than walk into it alone: half our ladder fights started outnumbered.
        if (!favourable && worker_danger(position).first) {
            worker_track_sightings(); // The camps by these enemies wait out the sighting.
            target = -1;
            if (worker_step_away())
                return true;
        }
        if (distance(position, victim.position) <= std::max(victim.range + 1, 4)) {
            retreating = health < max_health * 0.45f;
            if (retreating)
                return worker_escape();
            // Use a free ranged stun/poke on the way to the economic objective.
            combat(true, -1, true);
        }
        return false;
    }
    int d = distance(position, victim.position);
    if (!favourable && d > range && !kill)
        return false;
    auto &me = bc::self();
    bool moved = false;
    if (range <= 1 && d > range) {
        // Deliver available spell burst before a walk would consume the cast.
        // The combo evaluator still chooses a basic/reset when already in range.
        if (d <= ready_reach && combat(true))
            return true;
        moved = attack_dash(victim.position);
        moved = advance_attack(victim.position) || moved;
        if (moved)
            observe_fight();
    } else if (range > 1 && d > range) {
        if (d <= ready_reach && combat(true))
            return true;
        moved = attack_dash(victim.position);
        moved = advance_attack(victim.position) || moved;
        if (moved)
            observe_fight();
    }
    bool acted = combat(true);
    if (acted && me.is_alive() && range <= 1 && me.can_move())
        moved = follow_through() || moved;
    // A kill can refresh cooldowns (Vendetta), and the follow-through can bring
    // the next enemy into reach: act again with what is left of the turn.
    // Vendetta's damage to champions rose by a sixth.
    if (acted && me.is_alive() && range <= 1 && (me.can_attack() || me.can_cast())) {
        observe_fight();
        combat(true);
    }
    // Melee retains movement after attacking. Chase after a kill and reposition
    // out of splash clumps; ranged champions cannot walk after ordinary attacks.
    if (!acted && !moved && me.can_move())
        moved = advance_attack(victim.position);
    return acted || moved;
}
