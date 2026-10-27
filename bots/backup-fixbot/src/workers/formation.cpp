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
            // The squad a fight takes is the squad the enemy brought. Sizing it
            // by the power ratio (aggression 1.8) called a three-versus-five
            // meeting a two-body fight, so the two free champions were never
            // asked for and the three in the middle were killed.
            unsigned seen = 0;
            for (const auto &enemy : enemies)
                if (!enemy_spawn_tile[tile_index(enemy.position)])
                    ++seen;
            const unsigned bodies = std::max(1u, unsigned(roster.size()));
            reported_force = std::clamp(seen + 1, 1u, bodies);
            // Publish where the fight is, before the request that announces it.
            shared.set_rally(at, map_width, map_height);
            // Remembered so the call outlives a single round of lost vision: an
            // enemy that steps behind a bush ended the call before, and every
            // champion on its way turned back.
            call_round = int(round);
            call_group = reported_group;
            call_force = reported_force;
            call_rally = at;
        } else if (call_group >= 0 && int(round) - call_round <= policy::reinforce_call_rounds) {
            // Keep the call standing, at the last place the enemy was seen.
            reported_group = call_group;
            reported_force = call_force;
            shared.set_rally(call_rally, map_width, map_height);
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
    // Temporary instrumentation: one line per champion per round naming the
    // gate that stopped it, so the failure is measured instead of guessed.
    int calls = 0, attacked_calls = 0;
    if constexpr (policy::diagnostic)
        for (unsigned s = 0; s < std::min<unsigned>(roster.size(), TeamMemory::slots); ++s) {
            if (s == slot)
                continue;
            if (TeamMemory::requested_force(shared.get(s), groups.size()) > 0) {
                ++calls;
                attacked_calls += shared.attacked(s) ? 1 : 0;
            }
        }
    const auto report = [&](const char *why, int extra) {
        if constexpr (policy::diagnostic)
            std::printf("REINFORCE r=%u slot=%u pos=%d,%d seen=%zu calls=%d attcalls=%d extra=%d result=%s\n",
                        round, slot, position.x, position.y, enemies.size(), calls, attacked_calls, extra, why);
    };
    if (!enemies.empty()) {
        report("sees_enemy", 0);
        return false;
    }
    if (!TeamMemory::formations(groups.size())) {
        report("no_formations", 0);
        return false;
    }
    if (health < max_health * policy::retreat_health) {
        report("low_health", 0);
        return false;
    }
    if (target >= 0 && groups[target].status == CampGroup::capture && groups[target].tiles().is_set(position)) {
        report("capture_hold", target);
        return false;
    }
    const unsigned count = std::min<unsigned>(roster.size(), TeamMemory::slots);
    // A call is a summons. The old rule also demanded that the caller be
    // attackable on this exact tile; a meeting three tiles from the middle never
    // qualified, so the two outside champions farmed on while the three in the
    // middle were killed. The attacked bit now sets the pace instead: a hot
    // fight is chased urgently, a sighting is walked to.
    unsigned demand = 0;
    int leader_group = -1;
    bool hot = false;
    for (unsigned s = 0; s < count; ++s) {
        if (s == slot)
            continue;
        const unsigned code = shared.get(s);
        if (const unsigned force = TeamMemory::requested_force(code, groups.size())) {
            demand = std::max(demand, force);
            hot = hot || shared.attacked(s);
            if (leader_group < 0)
                leader_group = TeamMemory::fight_group(code, groups.size());
        }
    }
    // A sighting is a call too: at five bodies the middle of the map is where
    // both teams meet, and two champions that stay on their camps lose the
    // meeting before any of them is hit. A three-body roster has no champion to
    // spare, so there only a caller an enemy can strike draws help.
    if (!hot && count < policy::reinforce_squad_bodies) {
        report("small_roster_sighting", 0);
        return false;
    }
    // Where the fight is: an ally that is in it and in view, exactly; else the
    // point of the published rally cell nearest us. Rallies are to a champion in
    // trouble, not to ground: a camp is where the fight is not.
    vec2 low{}, high{};
    shared.rally_bounds(map_width, map_height, low, high);
    vec2 rally{std::clamp(position.x, low.x, high.x), std::clamp(position.y, low.y, high.y)};
    if (rally == position)
        rally = shared.rally(map_width, map_height); // Already inside the cell.
    bool have_mate = false;
    for (std::size_t i = 0; i < allies.size(); ++i)
        if (TeamMemory::requested_force(shared.get(ally_slot[i]), groups.size())) {
            rally = allies[i].position;
            have_mate = true;
        }
    // A call that goes quiet for a round (a caller losing sight, or acting
    // later in the order than we did) must not call its help off: keep walking
    // to the last rally for a few rounds.
    const bool holding = join_round >= 0 && int(round) - join_round <= policy::reinforce_hold_rounds;
    if (!demand && !holding) {
        report("no_call", 0);
        return false;
    }
    if (!demand) {
        rally = join_rally;
        have_mate = false;
    }
    std::array<bool, TeamMemory::slots> alive{}, engaged{};
    for (unsigned s = 0; s < count; ++s)
        alive[s] = roster[s].is_alive();
    // Who is actually in the fight: the callers, the joiners on their way, and a
    // teammate in view that an enemy can strike. Standing near the fight is not
    // being in it - counting a champion farming eight tiles off as a body of the
    // squad made the squad look complete, and the ally that was being killed
    // was left to die beside a teammate that never moved.
    unsigned committed = 0;
    for (unsigned s = 0; s < count; ++s) {
        if (s == slot || !alive[s])
            continue;
        const unsigned code = shared.get(s);
        const unsigned force = TeamMemory::requested_force(code, groups.size());
        const bool joining = !force && TeamMemory::fight_group(code, groups.size()) >= 0;
        const bool in_fight = force || joining || shared.attacked(s);
        engaged[s] = in_fight;
        committed += engaged[s] ? 1u : 0u;
    }
    const unsigned wanted = std::min(count, std::max(1u, demand));
    if (committed >= wanted && !holding) {
        report("enough_bodies", int(committed));
        return false;
    }
    const unsigned need = wanted - committed;
    const int speed = std::max(1, int(stats.base_move_speed));
    const int walk = distance(position, rally);
    // The response is priced by the fastest route, not by tiles walked. On this
    // map the spawn often sits between a farmer and the fight, so a recall home
    // (five rounds of channel) plus the walk out arrives far sooner than walking
    // the whole way, and a teleport crystal shortens it again. Counting tiles
    // alone turned exactly those champions away at the horizon.
    const int home_walk = home_field.get(position);
    int best = walk;
    const bool recall_route =
        home_walk != bc::distance_field::unreachable && home_walk > 1 &&
        recall_turns * speed + distance(home_centre, rally) < best;
    if (recall_route)
        best = recall_turns * speed + distance(home_centre, rally);
    const bool crystal = bc::self().consumables()[bc::consumable_type::tp_crystal].count() > 0;
    const int via_crystal = int(rules::settings().tp_crystal_time) +
                            std::max(0, walk - int(rules::crystal_range));
    if (crystal && via_crystal < best)
        best = via_crystal;
    const int reach = std::max(range + 1, policy::reinforce_travel_turns * speed);
    if (best > reach) {
        report("horizon", best);
        return false;
    }
    // Every champion that can arrive in time answers; the squad count above
    // decides only whether the call is still short of bodies. Handing the call
    // to the nearest answerer kept the second and third nearest farming while
    // the first was killed, and once a recall route is in play "nearest" is not
    // a reliable judgement of who arrives first anyway.
    (void)need;
    // Take the quicker route: a recall home when spawn is closer to the fight
    // than walking is (that is the arrangement this map rewards), otherwise
    // walk. retreat_to_spawn() keeps its own safety checks and walks instead
    // when an enemy could break the channel.
    if (recall_route) {
        const vec2 before = position;
        retreat_to_spawn();
        if (position != before || bc::self().is_recalling()) {
            join_round = int(round);
            join_rally = rally;
            report("RECALL_HOME", best);
            return true;
        }
        // Neither recalled nor stepped: fall through and walk at the fight.
    }
    const bool moved = move_to(around(rally, 1), rally, hot, true);
    if (!moved) {
        // Never stand still on a call that cannot be walked: occupancy, a camp
        // body or a wall can block the step, and the farm this turn is better
        // than freezing, which is what an unconditional "joining" did before.
        report("blocked", best);
        return false;
    }
    join_round = int(round);
    join_rally = rally;
    if (leader_group >= 0)
        publish(TeamMemory::joining(unsigned(leader_group), groups.size()));
    observe_fight();
    combat(true);
    report("JOIN", best);
    return true;
}
