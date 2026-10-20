// Worker turn for Issac, Ryth and Varg: independent per-champion policy.
#include "../bot.h"
#include <cstdio>
#include <cstring>
#include <iostream>
#include <tuple>

bool Bot::worker_escape() {
    publish(TeamMemory::recall);
    if (bc::map::spawn_region(bc::team::ally).is_set(position) && health >= max_health) {
        retreating = false;
        return false;
    }
    retreat_to_spawn();
    return true;
}

void Bot::worker_report() {
    if constexpr (policy::diagnostic) {
        if ((int(round) - last_report >= 50 || stalls == 8) && bc::fuel_remaining() > 10000) {
            std::printf("STATE r=%u id=%u type=%d pos=%d,%d hp=%d gems=%u gold=%u target=%d retreat=%d "
                        "bank=%d fuel=%llu\n",
                        round, id(bc::self()), int(type), position.x, position.y, int(health), gems,
                        bc::self().gold_count(), target, retreating, banking,
                        (unsigned long long)bc::fuel_remaining());
            last_report = round;
        }
    }
}

// Workers earn gems. In pure-resource mode they never choose a fight: they
// step away from danger and leave offense and defense to the squad.
void Bot::play_worker_turn() {
    auto &me = bc::self();
    if (!me.is_alive()) {
        publish(TeamMemory::unavailable);
        buy_equipment();
        retreating = banking = false;
        target = -1;
        return;
    }
    buy_equipment();
    if (me.is_recalling() || me.is_teleporting()) {
        publish(TeamMemory::recall);
        return;
    }
    observe();
    if constexpr (policy::worker_pure_resource)
        worker_track_sightings();
    if (me.is_submitting_gems()) {
        publish(TeamMemory::bank);
        // Protect a valuable bag only when the remaining channel is lethal.
        if (health < incoming(position) * (me.remaining_channel_time() + 1)) {
            me.cancel_submit_gems();
            retreating = true;
            worker_escape();
        }
        return;
    }
    const bool at_spawn = bc::map::spawn_region(bc::team::ally).is_set(position);
    if (at_spawn && health >= max_health)
        retreating = false;
    // Health recovery precedes fighting, capture, bank travel and shopping.
    // Empty bags and a nearly cleared camp never waive this obligation.
    retreating |= health_recovery_needed();
    if (retreating && worker_escape())
        return;
    if (!me.can_move() && !me.can_attack() && !me.can_cast()) {
        publish(TeamMemory::unavailable);
        return;
    }
    worker_disrupt_tribute();
    if (position == previous)
        ++stalls;
    else
        stalls = 0;
    previous = position;
    worker_report();

    // A guaranteed winning deposit has higher value than any additional kill.
    if (gems && gems + bc::global_submitted_gems(bc::team::ally) >= bc::constants::winning_gems) {
        banking = true;
        worker_tribute();
        return;
    }
    if constexpr (policy::worker_pure_resource) {
        // Danger first: an approaching enemy ends farming for this turn.
        if (worker_disengage())
            return;
        if (target < 0 && bc::fuel_remaining() > policy::observation_reserve)
            target = worker_choose_group();
        if (worker_tribute())
            return;
    } else {
        worker_update_formation();
        // Select a resource before combat can return, so a contested route does
        // not leave this worker without an economic objective.
        if (target < 0 && bc::fuel_remaining() > policy::observation_reserve)
            target = worker_choose_group();
        if (squad.members && type == bc::champion_type::issac && gems && !enemies.empty() &&
            incoming(position) > 0) {
            banking = true;
            if (worker_tribute())
                return;
        }
        if (worker_skirmish())
            return;
        if (worker_tribute())
            return;
        if (worker_reinforce())
            return;
    }

    bool upgrade =
        me.gold_count() >= bc::item_type::power_shard_of_strength.cost() *
                               (type == bc::champion_type::stellar || type == bc::champion_type::heart_of_ruin
                                    ? policy::carry_shopping_shards
                                    : policy::shopping_shards) &&
        passive != bc::passive_type::remote_shop;
    bool about_to_capture = target >= 0 && groups[target].status == CampGroup::capture &&
                            distance(position, groups[target].centre) <= 2;
    bool nearly_clear = target >= 0 && groups[target].health < dps(groups[target]) * 3;
    if (!at_spawn && !about_to_capture && !nearly_clear && upgrade) {
        if (gems && altar_field.get(position) <= 5) {
            banking = true;
            if (worker_tribute())
                return;
        }
        retreating = true;
        if (worker_escape())
            return;
    }
    worker_farm();
}

void Bot::worker_priority() {
    // Priority applies next round. Put an available burst or finishing blow
    // early in the allied order instead of using a permanent champion order.
    if (!initialised || bc::fuel_remaining() < policy::planning_reserve)
        return;
    auto &me = bc::self();
    int priority = passive == bc::passive_type::splash_attack ? priority_tier::idle + 200
                   : type == bc::champion_type::willow        ? priority_tier::idle + 600
                                                              : priority_tier::idle - static_cast<int>(slot);
    if (me.is_alive() && !me.is_recalling() && !me.is_submitting_gems() && !me.is_teleporting() &&
        !enemies.empty()) {
        const auto abilities = me.abilities();
        std::array<bool, 2> ready{};
        for (unsigned i = 0; i < ready.size(); ++i)
            ready[i] = abilities[i].cooldown() <= 1;
        int best = 0;
        for (const auto &e : enemies) {
            if (!e.unit().is_visible())
                continue;
            const float hp = e.unit().health();
            if (hp <= 0)
                continue;
            const int d = distance(position, e.position);
            const float basic = d <= range ? strength : 0;
            float spell = 0;
            for (unsigned i = 0; i < spells.size(); ++i) {
                if (!ready[i])
                    continue;
                const auto &s = spells[i];
                if (s.kind == bc::ability_kind::attack_reset && d <= range)
                    spell = std::max(spell, strength);
                if ((s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                     s.kind == bc::ability_kind::aoe_damage) &&
                    d <= s.range + s.radius)
                    spell = std::max(spell, focus * s.modifier);
            }
            const float burst = (basic + spell) * e.mitigation;
            if (burst <= 0)
                continue;
            const bool lethal = burst >= hp + e.shield;
            const int tier = lethal ? priority_tier::finish : priority_tier::damage;
            const float value = lethal ? burst + 200 + e.gems * 7 : burst;
            best = std::max(best, tier + std::min(int(value), priority_tier::span));
        }
        if (best > 0)
            priority = best;
    }
    if (me.priority() != priority)
        me.set_priority(priority);
}
