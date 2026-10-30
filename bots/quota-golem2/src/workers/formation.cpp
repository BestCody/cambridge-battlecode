// Worker reinforcement requests and responses.
#include "../bot.h"

namespace {
float observed_power(const ObservedUnit &unit) {
    return std::sqrt(std::max(1.0f, unit.health + unit.shield + unit.ready_shield) *
                     (unit.damage_rate + unit.burst * 0.2f + unit.healing_rate) / unit.mitigation);
}
} // namespace

float Bot::team_average_power() const {
    // Unseen allies contribute their public starting kit. Visible allies use
    // their current health and upgrades; no hidden live state is queried.
    float team_power = std::sqrt(std::max(1.0f, health) * spell_dps(spells, strength, focus) / mitigation);
    for (std::size_t s = 0; s < roster.size(); ++s)
        if (s != slot && !(seen_slots >> s & 1))
            team_power += roster_power[s];
    for (const auto &ally : allies)
        team_power += observed_power(ally);
    return team_power / float(std::max<std::size_t>(1, roster.size()));
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
            for (std::size_t i = 0; i < groups.size(); ++i) {
                const int d = std::max(std::abs(at.x - group_cx[i]), std::abs(at.y - group_cy[i]));
                if (d < closest) {
                    closest = d;
                    reported_group = int(i);
                }
            }
            // The squad that beats what we see by the fight rule: its power
            // times aggression reaches theirs.
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
    int selected = -1;
    float best = -1;
    vec2 destination{};
    unsigned selected_force = 0;
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
        const int travel = std::max(0, distance(position, rally) - range);
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
            if (!claim || claim > groups.size())
                continue;
            vec2 where = groups[claim - 1].centre;
            if (seen_slots >> s & 1)
                for (std::size_t i = 0; i < allies.size(); ++i)
                    if (ally_slot[i] == s) {
                        where = allies[i].position;
                        break;
                    }
            const int other_travel = std::max(0, distance(where, rally) - range);
            free += other_travel <= horizon;
            if (other_travel < travel || (other_travel == travel && s < slot))
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
        }
    }
    if (selected < 0)
        return false;
    publish(TeamMemory::joining(selected, groups.size()));
    const bool moved = move_to(around(destination, 1), destination, false, true);
    if (moved) {
        observe_fight();
        combat(true);
    }
    if constexpr (policy::diagnostic)
        if (moved && round % policy::replan_period == slot % policy::replan_period)
            std::printf("FORMATION r=%u id=%u group=%d demand=%u\n", round, id(bc::self()), selected,
                        selected_force);
    return moved;
}
