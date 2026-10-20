// Job estimates: firing positions, clear and capture timing, interference.
#include "../bot.h"
#include <tuple>

const bc::distance_field &Bot::member_route(unsigned member) {
    auto &cache = member_routes[member];
    const auto *seen = member == slot ? nullptr : observed_member(member);
    const vec2 at = member == slot ? position : seen->position;
    if (cache.origin != at || cache.revision != terrain_revision) {
        auto obstacles = static_blocked | known_camp_obstacles;
        obstacles.set(at, false);
        bc::bitboard visited;
        cache.field.compute(obstacles, around(at, 0), visited);
        cache.origin = at;
        cache.revision = terrain_revision;
    }
    return cache.field;
}

vec2 Bot::objective_staging(int index) {
    auto &group = groups[index];
    if (group.approach_known)
        return group.approach;
    bc::bitboard candidates, fire;
    int reach = 0;
    for (unsigned i = 0; i < roster.size(); ++i)
        if (squad.contains(i))
            for (const auto &ability : bc::champion(opening.roster_types[i]).abilities()) {
                const auto spell = describe(ability);
                reach = std::max(reach, int(spell.range));
            }
    for (const auto &camp : group.camps) {
        candidates |= around(camp.position, reach);
        if (camp.stats.base_strength > 0)
            fire |= around(camp.position, camp.stats.base_attack_range);
    }
    candidates &= ~static_blocked & ~known_camp_obstacles & ~group.tiles & ~fire;
    vec2 best{-1, -1};
    int shortest = bc::distance_field::unreachable;
    for (auto tile : candidates.positions()) {
        const int d = home_field.get(tile);
        if (d < shortest ||
            (d == shortest && best.x >= 0 && std::pair{tile.y, tile.x} < std::pair{best.y, best.x})) {
            shortest = d;
            best = tile;
        }
    }
    group.approach_known = true;
    group.approach = best;
    return best;
}

float Bot::position_output(const ObservedUnit &member, vec2 tile, const CampGroup &group,
                           const bc::bitboard &sight, int arrival, int &ready) const {
    float basic = 0;
    std::array<float, 2> cast{};
    for (const auto &camp : group.camps) {
        const float hp = group.status == CampGroup::cooling ? camp.stats.base_health : camp.health;
        if (hp <= 0 || !sight.is_set(camp.position))
            continue;
        if (distance(tile, camp.position) <= member.range)
            basic = std::max(basic, std::min(member.strength * member.camp_multiplier, hp));
        for (unsigned a = 0; a < member.abilities.size(); ++a) {
            const auto &spell = member.abilities[a];
            if (spell.kind == bc::ability_kind::damage || spell.kind == bc::ability_kind::cc) {
                if (distance(tile, camp.position) <= spell.range)
                    cast[a] = std::max(cast[a],
                                       std::min(member.focus * spell.modifier * member.camp_multiplier, hp));
            }
        }
    }
    for (unsigned a = 0; a < member.abilities.size(); ++a) {
        const auto &spell = member.abilities[a];
        if (spell.kind != bc::ability_kind::aoe_damage)
            continue;
        // A centre shared by several monsters is still one cast candidate.
        // Deduplication keeps dense camps from repeating the same splash scan.
        bc::bitboard centres;
        for (const auto &camp : group.camps)
            if (camp.health > 0 || group.status == CampGroup::cooling)
                centres |= around(camp.position, spell.radius);
        centres &= sight & ~static_blocked & around(tile, spell.range);
        for (auto centre : centres.positions()) {
            if (bc::fuel_remaining() < policy::planning_reserve)
                return -1; // Retry this position; a partial estimate is not a plan.
            float value = 0;
            for (const auto &camp : group.camps) {
                const float remaining =
                    group.status == CampGroup::cooling ? camp.stats.base_health : camp.health;
                if (remaining > 0 && distance(centre, camp.position) <= spell.radius)
                    value += std::min(member.focus * spell.modifier * member.camp_multiplier, remaining);
            }
            cast[a] = std::max(cast[a], value);
        }
    }
    for (unsigned a = 0; a < cast.size(); ++a)
        if (member.abilities[a].kind == bc::ability_kind::attack_reset)
            cast[a] = basic;
    auto cooldowns = member.cooldowns;
    for (auto &cooldown : cooldowns)
        cooldown = std::max(0, cooldown - arrival);
    const int horizon = work_window();
    float damage = 0;
    ready = bc::distance_field::unreachable;
    for (int turn = 0; turn < horizon; ++turn) {
        const bool moved = turn == 0 && (arrival > 0 || member.movement_spent);
        const bool active = arrival + turn > member.stun_time && !member.channel;
        const bool attack = active && (!moved || member.range <= 1) &&
                            (turn || arrival || (member.can_attack && !member.completed));
        float dealt = attack ? basic : 0;
        int chosen = -1;
        if (active && (turn || arrival || (member.can_cast && !member.completed)))
            for (unsigned a = 0; a < cast.size(); ++a) {
                if (cooldowns[a] || (moved && member.abilities[a].exclusive))
                    continue;
                if (chosen < 0 || cast[a] > cast[chosen])
                    chosen = a;
            }
        if (chosen >= 0 && cast[chosen] > 0) {
            dealt += cast[chosen];
            cooldowns[chosen] = member.abilities[chosen].cooldown;
        }
        if (dealt > 0 && ready == bc::distance_field::unreachable)
            ready = arrival + turn;
        damage += dealt;
        for (auto &cooldown : cooldowns)
            cooldown = std::max(0, cooldown - 1);
    }
    return damage / horizon;
}

FiringPosition Bot::firing_position(unsigned member_slot, int index, bool siege, bc::bitboard reserved,
                                    FiringSearch &search) {
    const ObservedUnit self(*this);
    const auto &member = member_slot == slot ? self : *observed_member(member_slot);
    const auto &group = groups[index];
    const auto &cached_route = member_routes[member_slot];
    if ((search.stage == 1 || search.stage == 3) &&
        (cached_route.origin != member.position || cached_route.revision != terrain_revision) &&
        bc::fuel_remaining() < policy::observation_reserve)
        return search.best;
    if (search.stage == 0) {
        int reach = member.range;
        for (const auto &spell : member.abilities)
            if (spell.modifier > 0)
                reach = std::max(reach, int(spell.range + spell.radius));
        bc::bitboard candidates, fire;
        for (const auto &camp : group.camps) {
            if (camp.health <= 0 && group.status == CampGroup::alive)
                continue;
            candidates |= around(camp.position, reach);
        }
        if (siege)
            for (const auto &other : groups)
                if (other.status == CampGroup::alive)
                    for (const auto &camp : other.camps)
                        if (camp.health > 0 && camp.stats.base_strength > 0 &&
                            std::any_of(group.camps.begin(), group.camps.end(), [&](const auto &target) {
                                return distance(camp.position, target.position) <=
                                       reach + camp.stats.base_attack_range;
                            }))
                            fire |= around(camp.position, camp.stats.base_attack_range);
        candidates &= ~static_blocked & ~known_camp_obstacles & ~reserved & ~fire;
        search.tiles = candidates.positions();
        search.stage = 1;
        return search.best;
    }
    if (search.stage == 1) {
        const auto &route = member_route(member_slot);
        // Read only candidate distances. Exporting an entire large map for a
        // handful of firing tiles costs more than the search itself.
        while (search.cursor < search.tiles.size()) {
            if (bc::fuel_remaining() < policy::planning_reserve)
                return search.best;
            const auto tile = search.tiles[search.cursor++];
            const int path = route.get(tile);
            if (path != bc::distance_field::unreachable) {
                int nearest = bc::distance_field::unreachable;
                for (const auto &camp : group.camps)
                    if (camp.health > 0 || group.status == CampGroup::cooling)
                        nearest = std::min(nearest, distance(tile, camp.position));
                float output = nearest <= member.range ? member.strength : 0;
                for (const auto &spell : member.abilities) {
                    if (spell.kind == bc::ability_kind::attack_reset && nearest <= member.range)
                        output += member.strength / spell.cooldown;
                    else if (nearest <= spell.range + spell.radius)
                        output += member.focus * spell.modifier / spell.cooldown;
                }
                const float hp = group.status == CampGroup::cooling ? group.max_health : group.health;
                const float cost =
                    float(path) / member.move_speed + hp / std::max(1.0f, output * member.camp_multiplier);
                search.ranked.push_back({tile, path, nearest, cost});
            }
        }
        search.stage = 2;
        return search.best;
    }
    if (search.stage == 2) {
        const auto kept = std::min<std::size_t>(policy::firing_trials, search.ranked.size());
        std::partial_sort(search.ranked.begin(), search.ranked.begin() + kept, search.ranked.end(),
                          [](const auto &a, const auto &b) {
                              return std::tuple{a.cost, a.path, a.tile.y, a.tile.x} <
                                     std::tuple{b.cost, b.path, b.tile.y, b.tile.x};
                          });
        search.ranked.resize(kept);
        search.tiles.clear();
        for (const auto &candidate : search.ranked)
            search.tiles.push_back(candidate.tile);
        search.cursor = 0;
        search.stage = 3;
        return search.best;
    }
    const auto &route = member_route(member_slot);
    while (search.cursor < search.tiles.size()) {
        if (bc::fuel_remaining() < policy::observation_reserve)
            return search.best;
        const auto tile = search.tiles[search.cursor++];
        const int path = route.get(tile);
        if (path == bc::distance_field::unreachable)
            continue;
        FiringPosition candidate;
        candidate.tile = tile;
        candidate.arrival = (path + member.move_speed - 1) / member.move_speed;
        const auto sight = bc::champion(member.type).visible_tiles_from(tile);
        candidate.output = position_output(member, tile, group, sight, candidate.arrival, candidate.ready);
        if (candidate.output < 0) {
            --search.cursor;
            return search.best;
        }
        if (candidate.output <= 0)
            continue;
        for (const auto &other : groups)
            if (other.status == CampGroup::alive &&
                (&other == &group || other.health < other.max_health || other.attacked >= int(round) - 1))
                for (const auto &camp : other.camps)
                    if (camp.health > 0 && distance(tile, camp.position) <= camp.stats.base_attack_range)
                        candidate.exposure +=
                            camp.stats.base_strength * member.camp_mitigation * member.mitigation;
        const float value =
            candidate.output / (1 + candidate.ready + work_window() +
                                candidate.exposure * work_window() / std::max(1.0f, member.health));
        if (value > search.value) {
            search.value = value;
            search.best = std::move(candidate);
        }
        // Validate the best-ranked productive position for this member.
        // Spend the remaining turn on actions; unsafe tanking is checked jointly.
        if (search.best.tile.x >= 0) {
            search.cursor = search.tiles.size();
            break;
        }
    }
    search.best.evaluated = true;
    return search.best;
}

JobEstimate Bot::estimate_job(int index, ObjectiveSearch &search) {
    const unsigned available = living_core();
    if (search.camp != index || search.membership != available ||
        int(round) - search.started > work_window() * std::max(1, std::popcount(available)) * 2) {
        search = {};
        search.camp = index;
        search.started = round;
        search.membership = available;
        search.result.camp = index;
        search.result.assessed = round;
    }
    auto &result = search.result;
    const auto &group = groups[index];
    auto reject = [&]() {
        JobEstimate rejected;
        rejected.complete = true;
        search = {};
        return rejected;
    };
    const ObservedUnit self(*this);
    if (!group.approach_known) {
        if (bc::fuel_remaining() < policy::observation_reserve)
            return result;
        objective_staging(index);
    }
    while (search.next_member < roster.size()) {
        const unsigned i = search.next_member;
        const auto *member = i == slot ? &self : observed_member(i);
        if (!(available & (1u << i)) || !member || member->channel || member->health <= 0) {
            ++search.next_member;
            continue;
        }
        if (bc::fuel_remaining() < policy::planning_reserve)
            return result;
        bc::bitboard reserved;
        for (unsigned other = 0; other < i; ++other)
            if (result.participants & (1u << other))
                reserved.set(result.firing[other].tile);
        if (group.status == CampGroup::capture) {
            // Clearing is complete; validate capture access below instead.
            result.firing[i].tile = member->position;
        } else {
            result.firing[i] = firing_position(i, index, result.siege, reserved, search.firing);
            if (!result.firing[i].evaluated)
                return result;
            search.firing = {};
            if (result.firing[i].tile.x < 0) {
                if (!result.firing[i].evaluated && bc::fuel_remaining() < policy::planning_reserve)
                    return result;
                return reject();
            }
        }
        result.participants |= 1u << i;
        ++search.next_member;
        return result;
    }
    if (!result.participants)
        return reject();
    if (bc::fuel_remaining() < policy::planning_reserve)
        return result;
    const int capture =
        group.holding_since < 0
            ? rules::settings().capture_time + 1
            : std::max(1, rules::settings().capture_time + 1 - (int(round) - group.holding_since));
    auto obstacles = static_blocked | (known_camp_obstacles & ~group.tiles);
    bc::bitboard visited;
    if (search.stage == 0) {
        result.output = result.loss = 0;
        result.travel = 0;
        for (unsigned i = 0; i < roster.size(); ++i) {
            if (!(result.participants & (1u << i)))
                continue;
            const auto *member = i == slot ? &self : observed_member(i);
            if (!member || member->channel) {
                search = {};
                return JobEstimate{};
            }
            auto &place = result.firing[i];

            result.travel = std::max(result.travel, place.arrival + std::max(0, member->stun_time));
            float output = place.output;
            if (i == slot && clear_progress.camp == index)
                output *= clear_progress.efficiency;
            result.output += output;
        }
        const float hp = group.status == CampGroup::cooling ? group.max_health : group.health;
        if (hp > 0 && result.output <= 0)
            return reject();
        result.clear = hp / std::max(1.0f, result.output);
        std::array<float, TeamMemory::slots> tank_damage{};
        for (const auto &camp : group.camps) {
            if ((camp.health <= 0 && group.status != CampGroup::cooling) || camp.stats.base_strength <= 0)
                continue;
            int tank = -1, nearest = bc::distance_field::unreachable;
            for (unsigned i = 0; i < roster.size(); ++i)
                if (result.participants & (1u << i)) {
                    const int d = distance(result.firing[i].tile, camp.position);
                    if (d <= camp.stats.base_attack_range && d < nearest) {
                        tank = i;
                        nearest = d;
                    }
                }
            if (tank >= 0) {
                const auto &member = unsigned(tank) == slot ? self : *observed_member(tank);
                tank_damage[tank] +=
                    camp.stats.base_strength * member.camp_mitigation * member.mitigation * result.clear;
            }
        }
        for (unsigned i = 0; i < roster.size(); ++i)
            if (result.participants & (1u << i)) {
                const auto &member = i == slot ? self : *observed_member(i);
                result.loss += tank_damage[i];
                if (tank_damage[i] >= member.health * (1 - policy::retreat_health)) {
                    if (result.siege)
                        return reject();
                    result = {};
                    result.camp = index;
                    result.siege = true;
                    search.next_member = 0;
                    search.firing = {};
                    return result;
                }
            }
        search.stage = 1;
        return result;
    }
    if (search.stage == 1) {
        // Allocate one capture position per planning slice. A nearest-target
        // search stops at the first free tile instead of flooding the map.
        while (search.capture_member < roster.size()) {
            const unsigned i = search.capture_member++;
            if (!(result.participants & (1u << i)))
                continue;
            auto &place = result.firing[i];
            bc::bitboard remaining = group.tiles & ~static_blocked;
            for (unsigned other = 0; other < i; ++other)
                if (result.firing[other].capture.x >= 0)
                    remaining.set(result.firing[other].capture, false);
            const bool overflow = remaining.is_empty();
            if (overflow)
                remaining = group.tiles & ~static_blocked;
            bc::distance_field from;
            from.compute_to(obstacles, around(place.tile, 0), remaining, visited);
            int nearest = bc::distance_field::unreachable;
            vec2 destination{-1, -1};
            for (auto tile : remaining.positions()) {
                const int path = from.get(tile);
                if (path < nearest) {
                    nearest = path;
                    destination = tile;
                }
            }
            if (destination.x < 0)
                return reject();
            const auto *member = i == slot ? &self : observed_member(i);
            if (!member || member->channel) {
                search = {};
                return JobEstimate{};
            }
            place.occupancy = (nearest + member->move_speed - 1) / member->move_speed;
            if (!overflow)
                place.capture = destination;
            // The trio waits for its slowest member; overflow members remain
            // adjacent rather than being assigned an occupied capture tile.
            search.occupancy = std::max(search.occupancy, place.occupancy);
            return result;
        }
        const int wait =
            group.status == CampGroup::cooling ? std::max(0, group.respawn - int(round) - result.travel) : 0;
        if (wait > rules::settings().capture_time)
            return reject();
        result.reward_time = result.travel + wait + int(std::ceil(result.clear)) + search.occupancy + capture;
        search.clear_reward_time = result.reward_time;
        search.stage = 2;
        return result;
    }
    for (unsigned i = 0; i < roster.size(); ++i)
        if ((result.participants & (1u << i)) && i != slot && !observed_member(i)) {
            search = {};
            return JobEstimate{};
        }
    if (search.stage == 2) {
        if (!enemies.empty() && bc::fuel_remaining() < policy::observation_reserve)
            return result;
        search.control_enemies.clear();
        search.next_enemy = 0;
        search.next_action = {};
        search.interference_work = search.exchange_loss = 0;
        result.control = true;
        // Hidden enemies are a bound, not an observation: price only part of
        // the overlap, and never let them veto the job outright.
        result.interference = hidden_interference(group);
        if (result.interference < search.clear_reward_time)
            search.interference_work +=
                (search.clear_reward_time - result.interference) * policy::hidden_interference_weight;
        for (const auto &enemy : enemies)
            search.control_enemies.push_back(id(std::get<bc::champion>(enemy.ref)));
        if (!enemies.empty())
            search.capture_routes.emplace().compute(obstacles, group.tiles & ~static_blocked, visited);
        for (unsigned i = 0; i < roster.size(); ++i) {
            if (!(result.participants & (1u << i)))
                continue;
            const auto &member = i == slot ? self : *observed_member(i);
            for (unsigned a = 0; a < member.cooldowns.size(); ++a)
                search.cast_ready[i][a] =
                    std::max(0, member.cooldowns[a] - (search.clear_reward_time - capture));
        }
        search.stage = 3;
        return result;
    }
    if (search.stage == 3) {
        // A changing visible opponent set invalidates the finite shared action
        // calendar. Resume only one contender per slice, never a whole-map
        // path search for every enemy in one actor turn.
        std::vector<unsigned> current;
        for (const auto &enemy : enemies)
            current.push_back(id(std::get<bc::champion>(enemy.ref)));
        if (current != search.control_enemies) {
            search.stage = 2;
            return result;
        }
        if (search.next_enemy < enemies.size()) {
            if (bc::fuel_remaining() < policy::observation_reserve)
                return result;
            const auto &enemy = enemies[search.next_enemy++];
            const int path = search.capture_routes->get(enemy.position);
            if (path == bc::distance_field::unreachable)
                return result;
            const int arrival =
                (path + enemy.move_speed - 1) / enemy.move_speed + std::max(0, enemy.stun_time);
            result.interference = std::min(result.interference, arrival);
            if (arrival > search.clear_reward_time)
                return result;
            bc::distance_field enemy_route;
            enemy_route.compute_to(obstacles, around(enemy.position, 0), group.tiles, visited);
            const auto entries = group.tiles.positions();
            vec2 entry = entries.front();
            for (auto tile : entries)
                if (enemy_route.get(tile) < enemy_route.get(entry))
                    entry = tile;
            std::array<std::optional<ObservedUnit>, TeamMemory::slots> defenders;
            for (unsigned i = 0; i < roster.size(); ++i) {
                if (!(result.participants & (1u << i)))
                    continue;
                defenders[i] = i == slot ? self : *observed_member(i);
                auto &member = *defenders[i];
                const auto &place = result.firing[i];
                member.position = place.capture.x >= 0 ? place.capture : place.tile;
                member.sight = bc::champion(member.type).visible_tiles_from(member.position);
            }
            float hp_left = enemy.health + enemy.shield, dealt = 0;
            bool disabled = false;
            for (unsigned i = 0; i < roster.size() && hp_left > 0 && !disabled; ++i) {
                if (!defenders[i])
                    continue;
                auto &member = *defenders[i];
                const int reach = distance(member.position, entry);
                if (!member.sight.is_set(entry))
                    continue;
                std::array<bool, 2> legal{};
                for (unsigned a = 0; a < member.abilities.size(); ++a) {
                    const auto &spell = member.abilities[a];
                    if (spell.kind == bc::ability_kind::aoe_damage) {
                        const auto centres = around(entry, spell.radius) & member.sight & ~static_blocked &
                                             around(member.position, spell.range);
                        legal[a] = !centres.is_empty();
                    } else
                        legal[a] = reach <= spell.range;
                }
                while (search.next_action[i] < capture && hp_left > 0 && !disabled) {
                    const int turn = search.next_action[i]++;
                    float damage = reach <= member.range ? member.strength : 0;
                    int cast = -1;
                    float spell_damage = 0;
                    for (unsigned a = 0; a < member.abilities.size(); ++a) {
                        const auto &spell = member.abilities[a];
                        if (search.cast_ready[i][a] > turn)
                            continue;
                        if (spell.kind == bc::ability_kind::attack_reset && reach <= member.range) {
                            if (member.strength > spell_damage) {
                                cast = a;
                                spell_damage = member.strength;
                            }
                        } else if (legal[a] && (spell.kind == bc::ability_kind::damage ||
                                                spell.kind == bc::ability_kind::cc ||
                                                spell.kind == bc::ability_kind::aoe_damage)) {
                            if (spell.kind == bc::ability_kind::cc && !enemy.immune &&
                                turn + spell.duration >= capture && spell.duration > 0) {
                                cast = a;
                                spell_damage = member.focus * spell.modifier;
                                disabled = true;
                                break;
                            }
                            if (member.focus * spell.modifier > spell_damage) {
                                cast = a;
                                spell_damage = member.focus * spell.modifier;
                            }
                        }
                    }
                    if (cast >= 0) {
                        damage += spell_damage;
                        search.cast_ready[i][cast] = turn + member.abilities[cast].cooldown;
                    }
                    const float hit = std::min(hp_left, damage * enemy.mitigation);
                    hp_left -= hit;
                    dealt += hit;
                    if (!damage && cast < 0 && member.range < reach && member.spell_reach < reach)
                        break;
                }
            }
            if (hp_left > 0 && !disabled) {
                result.control = false;
                const float extra = hp_left / std::max(1.0f, dealt / capture);
                search.interference_work += extra;
                search.exchange_loss += enemy.damage_rate * extra;
            }

            return result;
        }
        search.stage = 4;
    }
    float remaining_health = -result.loss;
    for (unsigned i = 0; i < roster.size(); ++i)
        if (result.participants & (1u << i))
            remaining_health += i == slot ? health : observed_member(i)->health;
    if (search.exchange_loss >= remaining_health)
        return reject();
    result.reward_time = search.clear_reward_time + int(std::ceil(search.interference_work));
    int delivery = group.altar_distance;
    if (delivery == bc::distance_field::unreachable)
        for (unsigned a = 0; a < altar_access.size(); ++a)
            if (altar_access[a].prepared)
                for (const auto &camp : group.camps) {
                    const int entry = altar_access[a].entry_routes.get(camp.position);
                    if (entry != bc::distance_field::unreachable)
                        delivery =
                            std::min(delivery, entry + rules::settings().tp_crystal_time +
                                                   rules::settings().recall_time + group.home_distance);
                }
    if (delivery == bc::distance_field::unreachable)
        return reject();
    const unsigned occupants =
        std::max(1u, std::min(unsigned(std::popcount(result.participants)), group.tiles.count()));
    result.delivery = delivery + channel_duration((group.unit.gem_value() + occupants - 1) / occupants);
    if (int(round) + result.reward_time + result.delivery >= int(rules::settings().max_rounds))
        return reject();
    result.pressure = group.unit.gem_value() && group.enemy_distance != bc::distance_field::unreachable &&
                      group.enemy_distance <= group.home_distance + 2 * opening.spacing;
    float useful = group.unit.gem_value();
    if (squad.access_preparation) {
        if (!group.unit.gold_value())
            return reject();
        result.pressure = false;
        useful += gems;
        for (unsigned i = 0; i < roster.size(); ++i)
            if (squad.contains(i))
                if (const auto *member = observed_member(i))
                    useful += member->gems;
    }
    if (!group.unit.gem_value() && group.unit.gold_value()) {
        const auto gold = bc::self().gold_count();
        const float gain =
            upgrade_gain(type, max_health, strength, focus, gold + group.unit.gold_value() / occupants) -
            upgrade_gain(type, max_health, strength, focus, gold);
        float continuation = 0;
        for (const auto &next : groups)
            if (next.unit.gem_value())
                continuation = std::max(continuation,
                                        next.unit.gem_value() / (1.0f + distance(group.centre, next.centre) /
                                                                            std::max(1.0f, opening.spacing)));
        useful += continuation * std::max(0.0f, gain) / (1 + std::max(0.0f, gain));
    }
    if (useful <= 0)
        return reject();
    result.rate = useful / std::max(1, result.reward_time + result.delivery);
    if (squad.enemy_pace > 0) {
        const float horizon = (rules::settings().winning_gems - squad.enemy_score) / squad.enemy_pace;
        result.rate *= std::min(1.0f, horizon / std::max(1, result.reward_time + result.delivery));
    }
    result.staging = group.approach;
    result.assessed = round;
    result.revision = terrain_revision;
    result.status = int(group.status);
    result.threats = relevant_threats();
    result.complete = true;
    const auto completed = result;
    search = {};
    return completed;
}

bool Bot::prepare_objective() {
    if (squad.camp < 0)
        return false;
    // Keep a plan until something it depends on changes: membership, terrain,
    // the camp's phase or a new threat. Age alone is only a long backstop.
    const auto &current = squad.plan;
    if (current.camp == squad.camp && current.complete && current.assessed >= 0 &&
        int(round) - current.assessed < work_window() * policy::plan_lifetime_windows &&
        current.revision == terrain_revision && current.status == int(groups[squad.camp].status) &&
        relevant_threats() <= current.threats &&
        (current.participants & living_core()) == current.participants)
        return true;
    if (bc::fuel_remaining() < policy::planning_reserve)
        return false;
    auto plan = estimate_job(squad.camp, active_search);
    while (!plan.complete && active_search.camp >= 0 && bc::fuel_remaining() >= policy::observation_reserve)
        plan = estimate_job(squad.camp, active_search);
    if (!plan.complete)
        return false;
    if (plan.camp < 0) {
        squad.next_job_review = round;
        return false;
    }
    squad.plan = plan;
    if (squad.arrival_deadline < 0)
        squad.arrival_deadline = round + plan.travel + work_window();
    return true;
}

void Bot::record_camp_damage(int camp, float damage) {
    if (!squad_member() || camp < 0 || damage <= 0)
        return;
    if (clear_progress.camp != camp) {
        clear_progress = {};
        clear_progress.camp = camp;
    }
    clear_progress.damage += damage;
}

void Bot::finish_clear_turn(int camp, float expected) {
    if (camp < 0 || groups[camp].status != CampGroup::alive || expected <= 0)
        return;
    if (clear_progress.camp != camp) {
        clear_progress = {};
        clear_progress.camp = camp;
    }
    if (clear_progress.last == int(round))
        return;
    clear_progress.last = round;
    clear_progress.expected += expected;
    ++clear_progress.rounds;
    if (clear_progress.rounds >= work_window()) {
        clear_progress.efficiency =
            std::min(1.0f, clear_progress.damage / std::max(1.0f, clear_progress.expected));
        if constexpr (policy::diagnostic)
            if (bc::fuel_remaining() >= policy::planning_reserve)
                std::printf("CLEAR_OUTPUT r=%u slot=%u camp=%d actual=%.1f expected=%.1f efficiency=%.2f\n",
                            round, slot, camp, clear_progress.damage, clear_progress.expected,
                            clear_progress.efficiency);
        clear_progress.damage = clear_progress.expected = 0;
        clear_progress.rounds = 0;
        squad.plan.assessed = -1;
        squad.next_job_review = round;
    }
}
