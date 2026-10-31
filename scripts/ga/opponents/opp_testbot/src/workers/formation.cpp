// Worker reinforcement requests and responses.
#include "../bot.h"
#include <wasm_simd128.h>

namespace {
float observed_power(const ObservedUnit &unit) {
    return unit_power(unit.health + unit.shield + unit.ready_shield, unit.damage_rate, unit.burst,
                      unit.healing_rate, unit.mitigation);
}
} // namespace

float Bot::team_average_power() const {
    // The champions who could come: living unseen allies contribute their
    // public starting kit, visible allies their current health and upgrades;
    // no hidden live state is queried. Dead allies counted at full power
    // (and in the count) sized squads that could not come.
    float team_power = own_power();
    unsigned living = 1 + unsigned(allies.size());
    for (std::size_t s = 0; s < roster.size(); ++s)
        if (s != slot && !(seen_slots >> s & 1) && roster[s].is_alive()) {
            team_power += roster_power[s];
            ++living;
        }
    for (const auto &ally : allies)
        team_power += observed_power(ally);
    return team_power / float(living);
}

bool Bot::same_fight(int group, vec2 at) const {
    // Reports name the group nearest to what each reporter sees: within one
    // champion's sight of each other they describe the same enemies.
    return group >= 0 && distance(groups[group].centre, at) <= int(stats.vision_radius);
}

unsigned Bot::fight_demand(vec2 at) const {
    // Each champion sees only its part of a fight: the squad it takes is the
    // largest any reporter asks for.
    unsigned demand = same_fight(reported_group, at) ? reported_force : 0;
    const unsigned count = std::min<unsigned>(roster.size(), TeamMemory::slots);
    for (unsigned s = 0; s < count; ++s) {
        if (s == slot)
            continue;
        const unsigned code = shared.get(s);
        const unsigned force = TeamMemory::requested_force(code, groups.size());
        if (force > demand && same_fight(TeamMemory::fight_group(code, groups.size()), at))
            demand = force;
    }
    return demand;
}

float Bot::reported_power(vec2 at) const {
    // Teammates' reports of this fight, as the enemy power that sized them.
    unsigned force = 0;
    const unsigned count = std::min<unsigned>(roster.size(), TeamMemory::slots);
    for (unsigned s = 0; s < count; ++s) {
        if (s == slot)
            continue;
        const unsigned code = shared.get(s);
        const unsigned asked = TeamMemory::requested_force(code, groups.size());
        if (asked > force && same_fight(TeamMemory::fight_group(code, groups.size()), at))
            force = asked;
    }
    return force ? float(force) * policy::aggression * team_average_power() : 0.0f;
}

bool Bot::worker_update_formation() {
    profiling::Scope scope(profiling::Stage::formation);
    // Worked out once a round: a stunned or retreating champion reports the
    // same fight again.
    if (reported_round != int(round)) {
        reported_round = int(round);
        reported_group = -1;
        reported_force = 0;
        const ObservedUnit *contact = nullptr;
        int closest = bc::distance_field::unreachable;
        if (TeamMemory::formations(groups.size()))
            for (const auto &enemy : enemies) {
                if (enemy_spawn_tile[tile_index(enemy.position)])
                    continue;
                const int d = distance(position, enemy.position);
                if (d < closest) {
                    contact = &enemy;
                    closest = d;
                }
            }
        if (contact) {
            closest = bc::distance_field::unreachable;
            const vec2 at = contact->position;
            // The nearest centre, the first of equals: four groups a step,
            // each lane keeping its own first nearest, then the lanes' best
            // (the lower group of equals) and the rest one by one.
            const std::size_t count = groups.size(), whole = count & ~std::size_t{3};
            if (whole) {
                const v128_t x = wasm_i32x4_splat(at.x), y = wasm_i32x4_splat(at.y);
                v128_t best = wasm_i32x4_splat(std::numeric_limits<std::int32_t>::max());
                v128_t index = wasm_i32x4_make(0, 1, 2, 3), best_index = index;
                const v128_t step = wasm_i32x4_splat(4);
                for (std::size_t i = 0; i < whole; i += 4) {
                    const v128_t d =
                        wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(x, wasm_v128_load(&group_cx[i]))),
                                       wasm_i32x4_abs(wasm_i32x4_sub(y, wasm_v128_load(&group_cy[i]))));
                    const v128_t nearer = wasm_i32x4_lt(d, best);
                    best = wasm_v128_bitselect(d, best, nearer);
                    best_index = wasm_v128_bitselect(index, best_index, nearer);
                    index = wasm_i32x4_add(index, step);
                }
                alignas(16) std::int32_t lane_d[4], lane_i[4];
                wasm_v128_store(lane_d, best);
                wasm_v128_store(lane_i, best_index);
                for (int l = 0; l < 4; ++l)
                    if (lane_d[l] < closest || (lane_d[l] == closest && lane_i[l] < reported_group)) {
                        closest = lane_d[l];
                        reported_group = lane_i[l];
                    }
            }
            for (std::size_t i = whole; i < count; ++i) {
                const int d = std::max(std::abs(at.x - group_cx[i]), std::abs(at.y - group_cy[i]));
                if (d < closest) {
                    closest = d;
                    reported_group = int(i);
                }
            }
            // Size the reported squad from enemy power and our average
            // champion power, scaled by the formation aggression factor.
            const unsigned bodies = std::max(1u, unsigned(roster.size()));
            const float squad = enemy_power(at) / (policy::aggression * std::max(1.0f, team_average_power()));
            reported_force = std::clamp(unsigned(std::ceil(squad)), 1u, bodies);
        }
    }
    if (reported_group < 0) {
        if (TeamMemory::fight_group(shared.get(slot), groups.size()) >= 0)
            publish(0);
        return false;
    }
    publish(TeamMemory::request(reported_group, reported_force, groups.size()));
    return true;
}

bool Bot::worker_reinforce() {
    profiling::Scope scope(profiling::Stage::reinforce);
    if (!enemies.empty() || !TeamMemory::formations(groups.size()) ||
        health < max_health * policy::retreat_health)
        return false;
    if (target >= 0 && groups[target].status == CampGroup::capture && groups[target].tiles().is_set(position))
        return false;
    const unsigned count = std::min<unsigned>(roster.size(), TeamMemory::slots);
    // Willow keeps a chosen real fight instead of bouncing back to a camp ally
    // whenever its attacked flag flickers. A recall route keeps the same intent
    // through its channel. Both follow non-attacked sightings of that contact
    // and end only when the recall fails or every matching report disappears.
    if (reinforce_commitment >= 0) {
        if (!reinforce_commitment_ready) {
            if (own_spawn.is_set(position))
                reinforce_commitment_ready = true;
            else {
                reinforce_commitment = -1;
                reinforce_commitment_ready = false;
            }
        }
        if (reinforce_commitment >= 0) {
            const vec2 remembered = groups[reinforce_commitment].centre;
            int active_group = -1;
            vec2 rally = remembered;
            for (unsigned s = 0; s < count; ++s) {
                if (s == slot || !roster[s].is_alive())
                    continue;
                const unsigned code = shared.get(s);
                if (!TeamMemory::requested_force(code, groups.size()))
                    continue;
                const int group = TeamMemory::fight_group(code, groups.size());
                if (!same_fight(group, remembered))
                    continue;
                active_group = group;
                if (s < slot_ally.size() && slot_ally[s] >= 0)
                    rally = allies[std::size_t(slot_ally[s])].position;
                break;
            }
            if (active_group >= 0) {
                reinforce_commitment = active_group;
                publish(TeamMemory::joining(active_group, groups.size()));
                const bool moved = move_to(around(rally, 1), rally, false, true);
                if (moved)
                    observe_fight();
                combat(true); // Also beside the ally: a heal needs no enemy in view.
                // We advertised commitment, so hold this turn even if a
                // temporary occupancy conflict prevented the planned step.
                return true;
            }
            reinforce_commitment = -1;
            reinforce_commitment_ready = false;
        }
    }
    // Help goes to fights where a teammate is under attack, not to sightings.
    bool requested = false;
    for (unsigned s = 0; s < count; ++s)
        requested |=
            s != slot && shared.attacked(s) && TeamMemory::requested_force(shared.get(s), groups.size()) > 0;
    if (!requested)
        return false;
    std::array<bool, TeamMemory::slots> alive{};
    for (unsigned s = 0; s < count; ++s)
        alive[s] = roster[s].is_alive();
    const float horizon = std::max(float(range + 1), opening.spacing);
    struct Travel {
        float distance;
        bool recall;
    };
    const auto has_dash = [&](unsigned s) {
        if (s >= roster_kits.size())
            return false;
        for (const auto &spell : roster_kits[s].spells)
            if (spell.kind == bc::ability_kind::dash)
                return true;
        return false;
    };
    const auto walk_to_fight = [&](vec2 from, vec2 rally, int group) {
        const int bound = walk_bound(from, group);
        return bound == bc::distance_field::unreachable ? bound : std::max(distance(from, rally), bound);
    };
    // Compare direct travel with recall plus the exact spawn-to-fight walk.
    // Distances remain in the old horizon's tile units: recall rounds become
    // the tiles otherwise walked in that time, and dash uses farming's scale.
    const auto support_travel = [&](int direct_walk, vec2 rally, int attack_range, int move_speed, bool dash,
                                    int recall_wait, bool on_spawn, bool allow_recall) {
        const float scale = dash ? policy::dash_travel_scale : 1.0f;
        const float direct = direct_walk == bc::distance_field::unreachable
                                 ? std::numeric_limits<float>::infinity()
                                 : float(std::max(0, direct_walk - attack_range)) * scale;
        float recalled = std::numeric_limits<float>::infinity();
        const int home_walk = home_field.get(rally);
        if (allow_recall && home_walk != bc::distance_field::unreachable)
            recalled = float(on_spawn ? 0 : std::max(0, recall_wait) * std::max(1, move_speed)) +
                       float(std::max(0, home_walk - attack_range)) * scale;
        return recalled < direct ? Travel{recalled, true} : Travel{direct, false};
    };
    const bool recall_is_safe = !hit && incoming(position) == 0;
    int selected = -1;
    float best = -1;
    vec2 destination{};
    unsigned selected_force = 0;
    bool selected_recall = false;
    for (unsigned leader = 0; leader < count; ++leader) {
        if (!alive[leader] || leader == slot || !shared.attacked(leader) ||
            !TeamMemory::requested_force(shared.get(leader), groups.size()))
            continue;
        const int group = TeamMemory::fight_group(shared.get(leader), groups.size());
        const vec2 place = groups[group].centre;
        const unsigned demand = fight_demand(place);
        vec2 rally = place;
        for (std::size_t i = 0; i < allies.size(); ++i)
            if (ally_slot[i] == leader) {
                rally = allies[i].position;
                break;
            }
        const Travel route = support_travel(walk_to_fight(position, rally, group), rally, range,
                                            int(stats.base_move_speed), has_dash(slot), recall_turns,
                                            own_spawn.is_set(position), recall_is_safe);
        const float travel = route.distance;
        if (travel > horizon)
            continue;
        // Teammates in this fight (under attack in it, or travelling to it),
        // the free ones who could also come in time, and those nearer than us.
        // One that only sees it may be stepping away: it counts as neither.
        unsigned committed = 0, free = 1, ahead = 0;
        for (unsigned s = 0; s < count; ++s) {
            if (s == slot || !alive[s])
                continue;
            const unsigned claim = shared.get(s);
            if (same_fight(TeamMemory::fight_group(claim, groups.size()), place)) {
                committed += !TeamMemory::requested_force(claim, groups.size()) || shared.attacked(s);
                continue;
            }
            // A champion already channeling recall has no reliable remaining
            // time in shared memory, so count it only after it publishes a new
            // camp objective from home. For ordinary farmers, predict the same
            // direct-versus-recall choice they will make on their turn.
            if (!claim || claim > groups.size())
                continue;
            const auto design = roster[s].base_stats();
            vec2 where = groups[claim - 1].centre;
            int mate_range = int(design.base_attack_range);
            int mate_speed = std::max(1, int(design.base_move_speed));
            bool mate_dash = has_dash(s), mate_on_spawn = false, mate_can_recall = true;
            if (s < slot_ally.size() && slot_ally[s] >= 0) {
                const auto &mate = allies[std::size_t(slot_ally[s])];
                where = mate.position;
                mate_range = mate.range;
                mate_speed = std::max(1, mate.move_speed);
                mate_dash = mate.dash_range > 0;
                mate_on_spawn = own_spawn.is_set(where);
                mate_can_recall = !mate.channel && incoming(where) == 0;
            }
            const Travel other = support_travel(walk_to_fight(where, rally, group), rally, mate_range, mate_speed,
                                                mate_dash, rules::settings().recall_time, mate_on_spawn,
                                                mate_can_recall);
            free += other.distance <= horizon;
            if (other.distance < travel || (other.distance == travel && s < slot))
                ++ahead;
        }
        // Nobody goes to a fight the team cannot make up the squad for.
        if (committed + free < demand || committed >= demand || ahead >= demand - committed)
            continue;
        const float value = float(demand - committed) / (travel + 1);
        if (value > best) {
            best = value;
            selected = group;
            destination = rally;
            selected_force = demand;
            selected_recall = route.recall;
        }
    }
    if (selected < 0)
        return false;
    if (selected_recall && !own_spawn.is_set(position)) {
        bc::self().recall();
        publish(TeamMemory::recall);
        recall_tile = position;
        reinforce_commitment = selected;
        reinforce_commitment_ready = false;
        return true;
    }
    reinforce_commitment = type == bc::champion_type::willow ? selected : -1;
    reinforce_commitment_ready = reinforce_commitment >= 0;
    publish(TeamMemory::joining(selected, groups.size()));
    const bool moved = move_to(around(destination, 1), destination, false, true);
    if (moved)
        observe_fight();
    if (moved || reinforce_commitment >= 0)
        combat(true); // Also a held turn beside the ally: a heal needs no enemy in view.
    if constexpr (policy::diagnostic)
        if (moved && round % policy::replan_period == slot % policy::replan_period)
            std::printf("FORMATION r=%u id=%u group=%d demand=%u\n", round, id(bc::self()), selected,
                        selected_force);
    // A committed Willow holds the response even if occupancy blocks this
    // step; farming must not pull her back toward a camp-wounded teammate.
    return moved || reinforce_commitment >= 0;
}
