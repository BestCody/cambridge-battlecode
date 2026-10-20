// Worker reinforcement requests and responses.
#include "../bot.h"

namespace {
float observed_power(const ObservedUnit &unit) {
    return std::sqrt(std::max(1.0f, unit.health + unit.shield + unit.ready_shield) *
                     (unit.damage_rate + unit.retaliation() + unit.burst * 0.2f + unit.healing_rate) /
                     unit.mitigation);
}
} // namespace

void Bot::worker_update_formation() {
    if (!TeamMemory::formations(groups.size()) || bc::fuel_remaining() < policy::observation_reserve)
        return;
    if (TeamMemory::fight_group(shared.get(slot), groups.size()) >= 0)
        publish(0);
    const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    const ObservedUnit *contact = nullptr;
    int closest = bc::distance_field::unreachable;
    for (const auto &enemy : enemies) {
        if (enemy_spawn.is_set(enemy.position))
            continue;
        const int d = distance(position, enemy.position);
        if (d < closest) {
            contact = &enemy;
            closest = d;
        }
    }
    if (!contact)
        return;
    int contact_group = -1;
    closest = bc::distance_field::unreachable;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const int d = distance(contact->position, groups[i].centre);
        if (d < closest) {
            closest = d;
            contact_group = int(i);
        }
    }
    float team_power = 0;
    const float splash = passive == bc::passive_type::splash_attack ? opening.mean_splash : 1;
    const float own_power =
        std::sqrt(std::max(1.0f, health) * spell_dps(spells, strength, focus, splash) / mitigation);
    // Unseen allies contribute their public starting kit. Visible allies use
    // their current health and upgrades; no hidden live state is queried.
    for (std::size_t s = 0; s < roster.size(); ++s) {
        float power = s == slot ? own_power : roster_power[s];
        for (const auto &ally : allies)
            if (std::get<bc::champion>(ally.ref) == roster[s]) {
                power = observed_power(ally);
                break;
            }
        team_power += power;
    }
    const unsigned bodies = std::max(1u, unsigned(roster.size()));
    const float average = team_power / bodies;
    const unsigned contact_force =
        std::clamp(unsigned(std::ceil(enemy_power(contact->position) * policy::formation_advantage /
                                      std::max(1.0f, average))),
                   1u, bodies);
    publish(TeamMemory::request(contact_group, contact_force, groups.size()));
}

bool Bot::worker_reinforce() {
    if (!enemies.empty() || !TeamMemory::formations(groups.size()) ||
        health < max_health * policy::retreat_health || bc::fuel_remaining() < policy::observation_reserve)
        return false;
    if (target >= 0 && groups[target].status == CampGroup::capture && groups[target].tiles.is_set(position))
        return false;
    const unsigned count = std::min<unsigned>(roster.size(), TeamMemory::slots);
    bool requested = false;
    for (unsigned s = 0; s < count; ++s)
        requested |= s != slot && TeamMemory::requested_force(shared.get(s), groups.size()) > 0;
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
        if (squad.contains(leader))
            continue; // Worker reinforcement stays within the resource pool.
        const unsigned demand = TeamMemory::requested_force(shared.get(leader), groups.size());
        if (!alive[leader] || !demand || leader == slot)
            continue;
        const int group = TeamMemory::fight_group(shared.get(leader), groups.size());
        vec2 rally = groups[group].centre;
        for (const auto &ally : allies)
            if (std::get<bc::champion>(ally.ref) == roster[leader]) {
                rally = ally.position;
                break;
            }
        const int travel = std::max(0, distance(position, rally) - range);
        if (travel > horizon)
            continue;
        unsigned committed = 0, ahead = 0;
        for (unsigned s = 0; s < count; ++s) {
            if (s == slot || !alive[s])
                continue;
            const unsigned claim = shared.get(s);
            if (TeamMemory::fight_group(claim, groups.size()) == group) {
                ++committed;
                continue;
            }
            if (!claim || claim > groups.size())
                continue;
            vec2 where = groups[claim - 1].centre;
            for (const auto &ally : allies)
                if (std::get<bc::champion>(ally.ref) == roster[s]) {
                    where = ally.position;
                    break;
                }
            const int other_travel = std::max(0, distance(where, rally) - range);
            if (other_travel < travel || (other_travel == travel && s < slot))
                ++ahead;
        }
        if (committed >= demand || ahead >= demand - committed)
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
    const bool moved = move_to(around(destination, 1), false, true);
    if (moved && bc::fuel_remaining() > policy::observation_reserve) {
        observe_fight();
        combat(true);
    }
    if constexpr (policy::diagnostic)
        if (moved && round % policy::replan_period == slot % policy::replan_period &&
            bc::fuel_remaining() > 6000)
            std::printf("FORMATION r=%u id=%u group=%d demand=%u\n", round, id(bc::self()), selected,
                        selected_force);
    return moved;
}
