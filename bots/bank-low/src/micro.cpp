#include "bot.h"

namespace {
struct Strike {
    int slot = -1, target = -1;
    vec2 tile{};
    std::uint64_t hits = 0, origins = 1;
    float damage = 0, shield = 0, support = 0;
    bool stun = false, reset = false, enhanced = false, heal = false;
};
struct Sequence {
    std::array<int, 3> actions{-1, -1, -1};
    float score = 0;
    int focus = -1, origin = 0;
};
} // namespace

bool Bot::attack_combo(bool escaping) {
    // Search one turn's attack/cast combinations. Host queries stay outside the
    // search; equivalent area centres collapse to one set of affected enemies.
    if (enemies.empty() || enemies.size() > std::numeric_limits<std::uint64_t>::digits ||
        bc::fuel_remaining() < policy::observation_reserve)
        return false;
    auto &me = bc::self();
    const auto abilities = me.abilities();
    const bool attack_available = me.can_attack();
    const bool can_attack = attack_available && (!escaping || range <= 1);
    const bool can_step = !escaping && !walked && range <= 1 && me.can_move();
    int reach = can_attack ? range : 0;
    for (unsigned i = 0; i < spells.size(); ++i) {
        const auto &s = spells[i];
        if (!abilities[i].is_ready() || (walked && s.exclusive))
            continue;
        if (s.kind == bc::ability_kind::attack_reset && !attack_available)
            reach = std::max(reach, range);
        else if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                 s.kind == bc::ability_kind::aoe_damage)
            reach = std::max(reach, int(s.range + s.radius));
    }
    if (can_step)
        reach = std::max(reach, range + int(stats.base_move_speed));
    if (std::none_of(enemies.begin(), enemies.end(),
                     [&](const auto &e) { return e.health > 0 && distance(position, e.position) <= reach; }))
        return false;
    const auto buffs = me.buffs();
    const float initial_shield = buffs.has_shield() ? buffs.get<bc::buff_shield>().value : 0;
    const bool initially_enhanced = buffs.has_enhanced_attack();
    const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    std::vector<vec2> origins{position};
    std::vector<bc::bitboard> sight{bc::vision::visible_tiles()};
    std::vector<float> pressure{incoming(position)};
    if (can_step) {
        for (auto tile : blocked.reachable_tiles(position, stats.base_move_speed).positions()) {
            if (bc::fuel_remaining() < policy::planning_reserve)
                break;
            if (tile == position || origins.size() >= std::numeric_limits<std::uint64_t>::digits)
                continue;
            const bool new_attack = std::any_of(enemies.begin(), enemies.end(), [&](const auto &e) {
                return e.health > 0 && !enemy_spawn.is_set(e.position) &&
                       distance(position, e.position) > range && distance(tile, e.position) <= range;
            });
            if (!new_attack)
                continue;
            origins.push_back(tile);
            sight.push_back(me.visible_tiles_from(tile));
            pressure.push_back(incoming(tile));
        }
    }
    const std::uint64_t every_origin = origins.size() == std::numeric_limits<std::uint64_t>::digits
                                           ? ~std::uint64_t{0}
                                           : (std::uint64_t{1} << origins.size()) - 1;
    auto in_range = [&](vec2 target, float reach, bool exclusive, bool needs_sight = true) {
        std::uint64_t mask = 0;
        const std::size_t count = exclusive ? 1 : origins.size();
        for (std::size_t i = 0; i < count; ++i)
            if (distance(origins[i], target) <= reach &&
                (!needs_sight || sight[i].is_set(target)))
                mask |= std::uint64_t{1} << i;
        return mask;
    };
    std::uint64_t eligible = 0;
    std::vector<float> original, remaining, weights, follow_up;
    original.reserve(enemies.size());
    weights.reserve(enemies.size());
    // Concentrate the volley where it removes the most offensive power per
    // health point. Normalize within the visible fight so ordinary damage,
    // control and healing remain on comparable scales across army sizes.
    float total_threat = 0, total_health = 0;
    for (const auto &e : enemies)
        if (e.health > 0 && !enemy_spawn.is_set(e.position)) {
            total_threat += e.damage_rate + e.burst * 0.5f;
            total_health += e.health + e.shield + e.ready_shield;
        }
    const float typical_density = total_threat / std::max(1.0f, total_health);
    for (std::size_t i = 0; i < enemies.size(); ++i) {
        const auto &e = enemies[i];
        original.push_back(e.health + e.shield);
        float weight = (1 + std::min(2.0f, e.gems / 100.0f)) * (1 + 0.6f * (1 - e.health / e.max_health));
        const float density = (e.damage_rate + e.burst * 0.5f) /
                              std::max(1.0f, e.health + e.shield + e.ready_shield);
        weight *= std::sqrt(density / std::max(0.001f, typical_density));
        if (shared.focus() == id(std::get<bc::champion>(e.ref)) + 1)
            weight *= policy::attack_focus_bonus;
        weights.push_back(weight);
        float assistance = 0;
        for (const auto &ally : allies) {
            if (ally.stunned || ally.channel)
                continue;
            const int d = distance(ally.position, e.position);
            if (d <= ally.range)
                assistance += ally.strength;
            if (d <= std::max(ally.range, ally.spell_reach))
                assistance += ally.burst;
        }
        follow_up.push_back(assistance * e.mitigation);
        if (e.health > 0 && !enemy_spawn.is_set(e.position))
            eligible |= std::uint64_t{1} << i;
    }
    if (!eligible)
        return false;
    remaining.resize(enemies.size());
    std::vector<Strike> actions;
    std::vector<int> basics{-1}, casts{-1};
    for (std::size_t i = 0; i < enemies.size(); ++i) {
        const auto legal = in_range(enemies[i].position, range, false);
        if (!(eligible & (std::uint64_t{1} << i)) || !legal)
            continue;
        Strike a;
        a.target = i;
        a.origins = legal;
        a.damage = strength;
        a.hits = std::uint64_t{1} << i;
        if (passive == bc::passive_type::splash_attack)
            for (std::size_t j = 0; j < enemies.size(); ++j)
                if (distance(enemies[i].position, enemies[j].position) <=
                    bc::constants::passive_abilities::varg_splash_radius)
                    a.hits |= std::uint64_t{1} << j;
        a.hits &= eligible;
        basics.push_back(actions.size());
        actions.push_back(a);
    }
    for (int slot_index = 0; slot_index < 2; ++slot_index) {
        if (bc::fuel_remaining() < policy::planning_reserve)
            break;
        const auto &spell = spells[slot_index];
        const auto &ability = abilities[slot_index];
        if (!ability.is_ready() || (walked && spell.exclusive))
            continue;
        Strike a;
        a.slot = slot_index;
        a.origins = spell.exclusive ? 1 : every_origin;
        a.damage = ability.damage_amount();
        a.stun = spell.duration > 0 &&
                 (spell.kind == bc::ability_kind::cc || spell.kind == bc::ability_kind::aoe_damage);
        auto add = [&](const Strike &strike) {
            casts.push_back(actions.size());
            actions.push_back(strike);
        };
        if (spell.kind == bc::ability_kind::attack_reset) {
            a.reset = true;
            a.enhanced = spell.enhanced;
            if (basics.size() > 1)
                add(a);
        } else if (spell.kind == bc::ability_kind::damage || spell.kind == bc::ability_kind::cc) {
            for (std::size_t i = 0; i < enemies.size(); ++i)
                if ((eligible & (std::uint64_t{1} << i)) &&
                    (a.origins = in_range(enemies[i].position, spell.range, spell.exclusive))) {
                    a.target = i;
                    a.hits = std::uint64_t{1} << i;
                    add(a);
                }
        } else if (spell.kind == bc::ability_kind::aoe_damage) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> seen;
            for (const auto &e : enemies) {
                if (bc::fuel_remaining() < policy::planning_reserve)
                    break;
                if (!in_range(e.position, spell.range + spell.radius, spell.exclusive, false))
                    continue;
                for (int dx = -int(spell.radius); dx <= int(spell.radius); ++dx)
                    for (int dy = -int(spell.radius); dy <= int(spell.radius); ++dy) {
                        if (bc::fuel_remaining() < policy::planning_reserve)
                            break;
                        a.tile = e.position + vec2{dx, dy};
                        a.origins = in_range(a.tile, spell.range, spell.exclusive, false);
                        if (!bc::map::is_in_bounds(a.tile) || !a.origins)
                            continue;
                        a.hits = 0;
                        for (std::size_t i = 0; i < enemies.size(); ++i)
                            if (distance(a.tile, enemies[i].position) <= spell.radius)
                                a.hits |= std::uint64_t{1} << i;
                        a.hits &= eligible;
                        const auto pattern = std::pair{a.hits, a.origins};
                        if (a.hits && std::find(seen.begin(), seen.end(), pattern) == seen.end()) {
                            seen.push_back(pattern);
                            add(a);
                        }
                    }
            }
        } else if (spell.kind == bc::ability_kind::shield) {
            a.shield = ability.shield_amount();
            float danger = incoming(position);
            a.support = std::min(a.shield, danger * std::max(1, spell.duration)) * 0.85f;
            if (health < danger * 4)
                a.support *= 2;
            if (initial_shield > 0)
                a.support *= 0.2f;
            add(a);
        } else if (spell.kind == bc::ability_kind::heal) {
            a.heal = true;
            for (std::size_t i = 0; i < allies.size(); ++i)
                if ((a.origins = in_range(allies[i].position, spell.range, spell.exclusive))) {
                    a.target = i;
                    a.support = ability.heal_amount(std::get<bc::champion>(allies[i].ref)) * 0.9f;
                    if (allies[i].health < allies[i].max_health * 0.5f)
                        a.support *= 2;
                    add(a);
                }
        }
    }
    Sequence best;
    auto evaluate = [&](std::array<int, 3> sequence) {
        std::uint64_t legal = every_origin;
        for (int index : sequence)
            if (index >= 0)
                legal &= actions[index].origins;
        if (!legal)
            return;
        int origin = 0;
        if (!(legal & 1)) {
            float least_pressure = std::numeric_limits<float>::infinity();
            for (std::size_t i = 1; i < origins.size(); ++i)
                if ((legal & (std::uint64_t{1} << i)) && pressure[i] < least_pressure) {
                    origin = i;
                    least_pressure = pressure[i];
                }
        }
        std::copy(original.begin(), original.end(), remaining.begin());
        float score = origin ? -policy::unused_cast_cost : 0;
        float own_health = health, own_shield = initial_shield;
        bool enhanced = initially_enhanced;
        std::uint64_t stunned = 0;
        for (int index : sequence) {
            if (index < 0)
                continue;
            const auto &a = actions[index];
            if (own_health <= 0)
                return;
            if (a.target >= 0 && !a.heal && remaining[a.target] <= 0)
                return; // A unit-targeted action cannot follow its target's death.
            if (a.reset) {
                enhanced = a.enhanced;
                score -= policy::unused_cast_cost;
                continue;
            }
            if (a.shield > 0 && origin) {
                float support =
                    std::min(a.shield, pressure[origin] * std::max(1, spells[a.slot].duration)) * 0.85f;
                if (health < pressure[origin] * 4)
                    support *= 2;
                if (initial_shield > 0)
                    support *= 0.2f;
                score += support;
            } else
                score += a.support;
            if (a.slot >= 0)
                score -= policy::unused_cast_cost; // Prefer saving a cast if it adds no value.
            own_shield = std::max(own_shield, a.shield);
            float recoil = 0;
            for (std::size_t i = 0; i < enemies.size(); ++i) {
                const auto bit = std::uint64_t{1} << i;
                if (!(a.hits & bit) || remaining[i] <= 0)
                    continue;
                const auto &e = enemies[i];
                const float damage = a.damage * e.mitigation;
                score += std::min(damage, remaining[i]) * weights[i];
                remaining[i] -= damage;
                if (damage > 0)
                    recoil += e.retaliation() * mitigation;
                if (remaining[i] <= 0) {
                    score += (200 + e.gems * 7 + e.strength * 3) * weights[i];
                    if (passive == bc::passive_type::reset_cooldowns_on_kill)
                        score += 60;
                } else if ((a.stun || (a.slot < 0 && enhanced && int(i) == a.target)) && !e.stunned &&
                           !e.immune)
                    stunned |= bit;
            }
            if (a.slot < 0)
                enhanced = false;
            const float absorbed = std::min(own_shield, recoil);
            own_shield -= absorbed;
            own_health -= recoil - absorbed;
            score -= recoil * (1 + std::min(2.0f, gems / 100.0f));
            if (own_health <= 0)
                score -= 200 + gems * 7 + strength * 3;
        }
        // Stun only earns credit if the opponent survives the whole combo.
        int focus_target = -1;
        float focus_value = 0;
        for (std::size_t i = 0; i < enemies.size(); ++i) {
            if ((stunned & (std::uint64_t{1} << i)) && remaining[i] > 0) {
                const auto &e = enemies[i];
                score +=
                    (e.strength + e.burst) * (escaping ? 3 : 1) + follow_up[i] * policy::stun_followup_credit;
                if (e.channel)
                    score += 120 + e.gems * 3;
            }
            if (remaining[i] > 0 && remaining[i] < original[i]) {
                float value = (original[i] - remaining[i]) * weights[i];
                if (remaining[i] <= follow_up[i]) {
                    const auto &e = enemies[i];
                    const float finish =
                        (200 + e.gems * 7 + e.strength * 3) * weights[i] * policy::assist_finish_credit;
                    score += finish;
                    value += finish;
                }
                if (value > focus_value) {
                    focus_value = value;
                    focus_target = i;
                }
            }
        }
        if (score > best.score)
            best = {sequence, score, focus_target, origin};
    };
    // Evaluate attacks alone first, then both cast orders. Resets get a second
    // independently selected attack, allowing a kill followed by a new target.
    for (int first : basics) {
        if (first >= 0 && !can_attack)
            continue;
        evaluate({first, -1, -1});
        for (int cast : casts) {
            if (cast < 0)
                continue;
            if (bc::fuel_remaining() < policy::planning_reserve)
                break;
            if (actions[cast].reset) {
                if (first < 0 && attack_available)
                    continue;
                for (int second : basics) {
                    if (bc::fuel_remaining() < policy::planning_reserve)
                        break;
                    if (second >= 0)
                        evaluate({first, cast, second});
                }
            } else {
                evaluate({first, cast, -1});
                evaluate({cast, first, -1});
            }
        }
    }
    if (best.score <= 0 || bc::fuel_remaining() < policy::action_reserve)
        return false;
    bool acted = false;
    if (best.origin) {
        if (!move_step(origins[best.origin]))
            return false;
        acted = true;
    }
    for (int index : best.actions) {
        if (index < 0 || !me.is_alive())
            continue;
        const auto &a = actions[index];
        if (a.slot < 0) {
            const auto &enemy = std::get<bc::champion>(enemies[a.target].ref);
            if (me.can_attack() && enemy.is_visible() && enemy.is_alive()) {
                me.attack(enemy);
                acted = true;
            }
        } else if (abilities[a.slot].is_ready()) {
            bool used = false;
            if (a.reset && me.can_attack())
                continue;
            if (a.reset || a.shield > 0)
                used = abilities[a.slot].use();
            else if (a.heal)
                used = abilities[a.slot].use(std::get<bc::champion>(allies[a.target].ref));
            else if (spells[a.slot].kind == bc::ability_kind::aoe_damage)
                used = abilities[a.slot].use(a.tile);
            else {
                const auto &enemy = std::get<bc::champion>(enemies[a.target].ref);
                if (enemy.is_visible() && enemy.is_alive())
                    used = abilities[a.slot].use(enemy);
            }
            acted |= used;
        }
    }
    refresh_self();
    if (acted && best.focus >= 0)
        shared.focus(id(std::get<bc::champion>(enemies[best.focus].ref)) + 1);
    return acted;
}

bool Bot::attack_dash(vec2 victim) {
    auto &me = bc::self();
    if (!me.can_attack() || bc::fuel_remaining() < policy::planning_reserve)
        return false;
    const int gap = distance(position, victim) - range;
    if (gap <= 0 || (range <= 1 && gap <= int(stats.base_move_speed)))
        return false; // Walking into melee preserves the cast for an attack reset.
    for (const auto &ability : me.abilities()) {
        if (!ability.is_dash() || !ability.is_ready())
            continue;
        const auto design = describe(ability);
        if (gap > design.range || (walked && design.exclusive))
            continue;
        auto occupied = static_blocked & ~bc::map::holes();
        occupied |= bc::vision::camp_tiles() | bc::vision::champion_tiles() | bc::vision::teleport_tiles();
        occupied.set(position, false);
        const auto landings = occupied.reachable_tiles(position, unsigned(design.range)) & ~blocked;
        vec2 best = position;
        float best_score = std::numeric_limits<float>::infinity();
        for (auto tile : landings.positions()) {
            if (bc::fuel_remaining() < policy::action_reserve + 6000)
                break;
            if (tile == position || distance(tile, victim) > range)
                continue;
            float score = incoming(tile) * 0.14f + float(distance(tile, victim));
            if (score < best_score) {
                best_score = score;
                best = tile;
            }
        }
        if (best != position && ability.use(best)) {
            refresh_self();
            walked |= design.exclusive;
            blocked = static_blocked | bc::vision::champion_tiles() | bc::vision::camp_tiles() |
                      bc::vision::teleport_tiles();
            blocked.set(position, false);
            return true;
        }
    }
    return false;
}

bool Bot::follow_through() {
    // Melee attacks leave a move available. Use it to line up the next attack
    // or reach the next victim immediately after a kill.
    if (bc::fuel_remaining() < policy::observation_reserve)
        return false;
    for (auto &e : enemies) {
        e.health = e.unit().is_visible() ? e.unit().health() : 0;
        if (e.health > 0) {
            const auto buffs = e.unit().buffs();
            e.shield = buffs.has_shield() ? buffs.get<bc::buff_shield>().value : 0;
        }
    }
    blocked = static_blocked | bc::vision::champion_tiles() | bc::vision::camp_tiles() |
              bc::vision::teleport_tiles();
    blocked.set(position, false);
    const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    auto value = [&](vec2 tile) {
        float best = 0;
        for (const auto &e : enemies) {
            if (e.health <= 0 || enemy_spawn.is_set(e.position))
                continue;
            const int arrival = std::max(0, distance(tile, e.position) - range);
            float damage = std::min(strength * e.mitigation, e.health + e.shield);
            if (passive == bc::passive_type::splash_attack)
                for (const auto &other : enemies)
                    if (&e != &other && other.health > 0 &&
                        distance(e.position, other.position) <=
                            bc::constants::passive_abilities::varg_splash_radius)
                        damage += std::min(strength * other.mitigation, other.health + other.shield);
            if (e.health + e.shield <= strength * e.mitigation)
                damage += 200 + e.gems * 7;
            best = std::max(best, damage / (1 + arrival));
        }
        return best;
    };
    vec2 best = position;
    float best_score = value(position);
    for (auto tile : blocked.reachable_tiles(position, stats.base_move_speed).positions()) {
        const float score = value(tile);
        if (score > best_score + 0.01f) {
            best = tile;
            best_score = score;
        }
    }
    return best != position && move_step(best);
}

bool Bot::advance_attack(vec2 victim) {
    auto &me = bc::self();
    if (!me.can_move() || distance(position, victim) <= range ||
        bc::fuel_remaining() < policy::planning_reserve)
        return false;
    // A visible fight needs a local approach, not a route across the whole map.
    // Reserve the remaining turn for the attack and reset after this move.
    const auto local_blocked = blocked | ~around(position, stats.vision_radius);
    const auto goals = around(victim, range) & ~local_blocked;
    if (goals.is_empty())
        return false;
    bc::distance_field field;
    bc::bitboard visited;
    field.compute_to(local_blocked, goals, around(position, 0), visited);
    if (field.get(position) == bc::distance_field::unreachable ||
        bc::fuel_remaining() < policy::action_reserve)
        return false;
    bool moved = false;
    if (bc::fuel_remaining() > policy::observation_reserve)
        moved = cast_mobility(field, false, -1);
    if (moved) {
        blocked = static_blocked | bc::vision::champion_tiles() | bc::vision::camp_tiles() |
                  bc::vision::teleport_tiles();
        blocked.set(position, false);
    }
    if (!me.can_move() || distance(position, victim) <= range)
        return moved;
    vec2 best = position;
    float best_score = std::numeric_limits<float>::infinity();
    for (auto tile : blocked.reachable_tiles(position, stats.base_move_speed).positions()) {
        if (field.get(tile) >= field.get(position))
            continue;
        const float score = float(field.get(tile)) * 10 + incoming(tile) * 0.14f;
        if (score < best_score) {
            best_score = score;
            best = tile;
        }
    }
    return (best != position && move_step(best)) || moved;
}

void Bot::observe_fight() {
    // After moving during a fight, only enemies and occupancy can affect this
    // turn's attacks. Rebuilding every camp and ally wastes the attack budget.
    refresh_self();
    blocked = static_blocked | bc::vision::champion_tiles() | bc::vision::camp_tiles() |
              bc::vision::teleport_tiles();
    blocked.set(position, false);
    std::vector<ObservedUnit> current;
    current.reserve(enemies.size());
    for (auto c : bc::vision::champions(bc::team::enemy)) {
        if (bc::fuel_remaining() < policy::planning_reserve)
            break;
        if (!c.is_alive())
            continue;
        const auto found = std::find_if(enemies.begin(), enemies.end(),
                                        [&](const auto &e) { return std::get<bc::champion>(e.ref) == c; });
        if (found == enemies.end())
            current.emplace_back(c);
        else {
            current.push_back(*found);
            auto &e = current.back();
            e.position = c.position();
            e.health = c.health();
            const auto buffs = c.buffs();
            e.shield = buffs.has_shield() ? buffs.get<bc::buff_shield>().value : 0;
            e.stunned = buffs.has_stun();
            e.stun_time = e.stunned ? buffs.get<bc::buff_stun>().duration : -1;
        }
    }
    enemies = std::move(current);
}

void Bot::attack_order() {
    // Priority applies next round. Put an available burst or finishing blow
    // early in the allied order instead of using a permanent champion order.
    if (!initialised || bc::fuel_remaining() < policy::planning_reserve)
        return;
    auto &me = bc::self();
    int priority = passive == bc::passive_type::splash_attack ? 1200
                   : type == bc::champion_type::willow        ? 1600
                                                              : 1000 - static_cast<int>(slot);
    if (me.is_alive() && !me.is_recalling() && !me.is_submitting_gems() && !me.is_teleporting() &&
        !enemies.empty()) {
        const auto abilities = me.abilities();
        std::array<bool, 2> ready{};
        for (unsigned i = 0; i < ready.size(); ++i)
            ready[i] = abilities[i].cooldown() <= 1;
        float best = 0;
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
            float burst = (basic + spell) * e.mitigation;
            if (burst >= hp + e.shield)
                burst += 200 + e.gems * 7;
            best = std::max(best, burst);
        }
        if (best > 0)
            priority = 2000 + int(std::min(best, 10000.0f));
    }
    if (me.priority() != priority)
        me.set_priority(priority);
}
