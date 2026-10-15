#include "bot.h"

namespace {
bool present(const ObservedUnit &u) {
    return u.unit().is_visible() && u.unit().health() > 0;
}
} // namespace

bool Bot::combat(bool fighting_champions, int farm_group, bool escaping) {
    if (bc::fuel_remaining() < policy::action_reserve)
        return false;
    auto &me = bc::self();
    if (me.is_teleporting() || me.is_recalling() || me.is_submitting_gems())
        return false;
    if (bc::fuel_remaining() < policy::action_reserve)
        return false;
    if (!me.can_attack() && !me.can_cast())
        return false;
    if (fighting_champions && attack_combo(escaping))
        return true;
    if (bc::fuel_remaining() < policy::action_reserve)
        return false;
    auto &targets = fighting_champions ? enemies : camps;
    const bool has_retaliation =
        fighting_champions &&
        std::any_of(targets.begin(), targets.end(), [](const auto &u) { return u.retaliation() > 0; });
    auto read_shield = [&]() {
        if (!has_retaliation)
            return 0.0f;
        const auto buffs = me.buffs();
        return buffs.has_shield() ? buffs.get<bc::buff_shield>().value : 0.0f;
    };
    float own_shield = read_shield();
    auto eligible = [&](const ObservedUnit &u) {
        return (fighting_champions || u.group == farm_group) && u.health > 0 &&
               !(fighting_champions && bc::map::spawn_region(bc::team::enemy).is_set(u.position));
    };
    auto weight = [&](const ObservedUnit &u, float damage, bool cc) {
        if (!eligible(u))
            return 0.0f;
        float effective = damage * (u.enemy() ? u.mitigation : camp_multiplier);
        float value = std::min(effective, u.health + u.shield);
        if (effective >= u.health + u.shield) {
            value += u.enemy() ? 200 + u.gems * 7 + u.strength * 3 : u.strength * 8 + 20;
            if (passive == bc::passive_type::reset_cooldowns_on_kill)
                value += 60;
        }
        if (u.enemy()) {
            value *= 1 + std::min(2.0f, u.gems / 100.0f);
            value *= 1 + 0.6f * (1 - u.health / u.max_health);
            if (u.channel && cc && !u.immune)
                value += 120 + u.gems * 3;
        } else
            value += u.strength * effective / std::max(1.0f, u.health) * 3;
        if (cc && !u.stunned && !u.immune)
            value += (u.strength + u.burst) * (escaping ? 3 : 1);
        if (u.retaliation() > 0 && effective > 0) {
            const float recoil = u.retaliation() * mitigation;
            value -= recoil * (1 + std::min(2.0f, gems / 100.0f));
            if (recoil >= health + own_shield)
                value -= 200 + gems * 7 + strength * 3;
        }
        return value;
    };
    auto basic_choice = [&]() -> int {
        if (!me.can_attack())
            return -1;
        auto attack_tiles = me.attack_tiles();
        float best = 0;
        int index = -1;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (bc::fuel_remaining() < policy::action_reserve)
                break;
            auto &u = targets[i];
            if (!eligible(u) || !attack_tiles.is_set(u.position) || !present(u))
                continue;
            float value = weight(u, strength, false);
            if (passive == bc::passive_type::splash_attack)
                for (std::size_t j = 0; j < targets.size(); ++j)
                    if (j != i && distance(u.position, targets[j].position) <=
                                      bc::constants::passive_abilities::varg_splash_radius)
                        value += weight(targets[j], strength, false);
            if (value > best) {
                best = value;
                index = i;
            }
        }
        return index;
    };
    bool acted = false;
    auto do_attack = [&]() {
        int index = basic_choice();
        if (index < 0)
            return false;
        auto &u = targets[index];
        const vec2 struck = u.position;
        me.attack(u.unit());
        acted = true;
        if (u.group >= 0)
            groups[u.group].attacked = round;
        // Splash and retaliation can change several units. Refresh health before
        // the reset selection so dead or overkilled units cannot consume actions.
        for (auto &t : targets)
            if (eligible(t) &&
                (t.position == struck ||
                 (passive == bc::passive_type::splash_attack &&
                  distance(struck, t.position) <= bc::constants::passive_abilities::varg_splash_radius)))
                t.health = t.unit().is_visible() ? t.unit().health() : 0;
        refresh_self();
        own_shield = read_shield();
        return true;
    };
    // Normal attack precedes resets. Ranged champions may also move, reset and
    // attack; the reset evaluator below accounts for that restored allowance.
    if (!escaping || range <= 1)
        do_attack();
    if (!me.is_alive())
        return acted;

    int best_slot = -1, best_target = -1;
    vec2 best_tile{};
    float best_score = 0;
    auto abilities = me.abilities();
    for (int slot_index = 0; slot_index < 2; ++slot_index) {
        if (bc::fuel_remaining() < policy::action_reserve)
            break;
        const auto &a = abilities[slot_index];
        const auto &s = spells[slot_index];
        if (!a.is_ready() || (walked && s.exclusive))
            continue;
        if (s.kind == bc::ability_kind::attack_reset) {
            if (me.can_attack())
                continue;
            float score = 0;
            for (const auto &u : targets)
                if (eligible(u) && distance(position, u.position) <= range && present(u)) {
                    float v = weight(u, strength, s.enhanced);
                    if (passive == bc::passive_type::splash_attack)
                        for (const auto &other : targets)
                            if (&other != &u && distance(u.position, other.position) <=
                                                    bc::constants::passive_abilities::varg_splash_radius)
                                v += weight(other, strength, false);
                    score = std::max(score, v);
                }
            if (score > best_score) {
                best_score = score;
                best_slot = slot_index;
                best_target = -1;
            }
        } else if (s.kind == bc::ability_kind::shield) {
            float danger = incoming(position, farm_group);
            float score = std::min(a.shield_amount(), danger * std::max(1, s.duration)) * 0.85f;
            if (health < danger * 4)
                score *= 2;
            if (me.buffs().has_shield())
                score *= 0.2f;
            if (score > best_score) {
                best_score = score;
                best_slot = slot_index;
                best_target = -1;
            }
        } else if (s.kind == bc::ability_kind::heal) {
            for (std::size_t i = 0; i < allies.size(); ++i) {
                const auto &u = allies[i];
                if (distance(position, u.position) > s.range)
                    continue;
                const auto &ally = std::get<bc::champion>(u.ref);
                if (!ally.is_visible() || !ally.is_alive())
                    continue;
                float score = a.heal_amount(ally) * 0.9f;
                if (u.health < u.max_health * 0.5f)
                    score *= 2;
                if (score > best_score) {
                    best_score = score;
                    best_slot = slot_index;
                    best_target = i;
                }
            }
        } else if (s.kind == bc::ability_kind::aoe_damage) {
            // Evaluate each unit's surrounding centres, not only occupied tiles:
            // radius-one spells can hit targets five tiles from the caster.
            float damage = a.damage_amount();
            for (const auto &u : targets)
                if (eligible(u) && distance(position, u.position) <= s.range + s.radius) {
                    int radius = static_cast<int>(s.radius);
                    for (int dx = -radius; dx <= radius; ++dx)
                        for (int dy = -radius; dy <= radius; ++dy) {
                            if (bc::fuel_remaining() < policy::action_reserve)
                                break;
                            vec2 tile = u.position + vec2{dx, dy};
                            if (!bc::map::is_in_bounds(tile) || distance(position, tile) > s.range ||
                                (bc::map::walls() | bc::map::holes() | bc::map::altars()).is_set(tile))
                                continue;
                            float score = 0;
                            for (const auto &other : targets)
                                if (distance(tile, other.position) <= s.radius)
                                    score += weight(other, damage, s.duration > 0);
                            if (score > best_score) {
                                best_score = score;
                                best_slot = slot_index;
                                best_target = -1;
                                best_tile = tile;
                            }
                        }
                }
        } else if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc) {
            if (fighting_champions ? !s.targets.contains(bc::ability_target_mask::enemies)
                                   : !s.targets.contains(bc::ability_target_mask::camps))
                continue;
            float damage = a.damage_amount();
            for (std::size_t i = 0; i < targets.size(); ++i) {
                const auto &u = targets[i];
                if (!eligible(u) || distance(position, u.position) > s.range || !present(u))
                    continue;
                float score = weight(u, damage, s.kind == bc::ability_kind::cc);
                if (score > best_score) {
                    best_score = score;
                    best_slot = slot_index;
                    best_target = i;
                }
            }
        }
    }
    if (best_slot >= 0 && best_score > 1 && bc::fuel_remaining() > policy::action_reserve) {
        const auto &a = abilities[best_slot];
        const auto &s = spells[best_slot];
        bool success = false;
        if (s.kind == bc::ability_kind::attack_reset || s.kind == bc::ability_kind::shield)
            success = a.use();
        else if (s.kind == bc::ability_kind::heal)
            success = a.use(std::get<bc::champion>(allies[best_target].ref));
        else if (s.kind == bc::ability_kind::aoe_damage)
            success = a.use(best_tile);
        else
            success = std::visit([&](const auto &u) { return a.use(u); }, targets[best_target].ref);
        if (success) {
            acted = true;
            if (farm_group >= 0)
                groups[farm_group].attacked = round;
            for (std::size_t i = 0; i < targets.size(); ++i) {
                auto &u = targets[i];
                if (eligible(u) && ((s.kind == bc::ability_kind::aoe_damage &&
                                     distance(u.position, best_tile) <= s.radius) ||
                                    ((s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc) &&
                                     int(i) == best_target)))
                    u.health = u.unit().is_visible() ? u.unit().health() : 0;
            }
            refresh_self();
            own_shield = read_shield();
            do_attack();
        }
    }
    return acted;
}

bool Bot::skirmish() {
    if (enemies.empty())
        return false;
    const auto abilities = bc::self().abilities();
    int ready_reach = range;
    for (unsigned i = 0; i < spells.size(); ++i)
        if (abilities[i].is_ready() && spells[i].modifier > 0 &&
            (spells[i].kind == bc::ability_kind::damage || spells[i].kind == bc::ability_kind::cc ||
             spells[i].kind == bc::ability_kind::aoe_damage))
            ready_reach = std::max(ready_reach, int(spells[i].range + spells[i].radius));
    int selected = -1;
    float best = -1;
    bool selected_contact = false;
    const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    for (std::size_t i = 0; i < enemies.size(); ++i) {
        const auto &e = enemies[i];
        if (enemy_spawn.is_set(e.position))
            continue;
        int d = distance(position, e.position);
        // Choose the current engagement among enemies we can actually damage.
        // A wounded runner outside our reach must not redirect the whole fight
        // while other enemies are already inside our firing line.
        const bool contact = d <= ready_reach ||
                             (range <= 1 && d <= range + int(stats.base_move_speed));
        if (selected_contact && !contact)
            continue;
        float value = (150 + e.gems * 5 + e.strength * 2) / (std::max(100.0f, e.health + e.shield) * (d + 2));
        if (e.channel)
            value *= 2;
        if (shared.focus() == id(std::get<bc::champion>(e.ref)) + 1)
            value *= 1.6f;
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
    float spell_burst = 0;
    for (unsigned i = 0; i < 2; ++i)
        if (abilities[i].is_ready()) {
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
    bool favourable = ours * policy::aggression >= theirs;
    if ((health < danger * 2.2f || !favourable) && !kill) {
        if (distance(position, victim.position) <= std::max(victim.range + 1, 4)) {
            retreating = health < max_health * 0.45f;
            if (retreating)
                return escape();
            // Use a free ranged stun/poke on the way to the economic objective.
            combat(true, -1, true);
        }
        return false;
    }
    int d = distance(position, victim.position);
    if (!favourable && d > range && !kill)
        return false;
    shared.focus(id(std::get<bc::champion>(victim.ref)) + 1);
    auto &me = bc::self();
    bool moved = false;
    if (range <= 1 && d > range) {
        // Deliver available spell burst before a walk would consume the cast.
        // The combo evaluator still chooses a basic/reset when already in range.
        if (d <= ready_reach && combat(true))
            return true;
        moved = attack_dash(victim.position);
        moved = advance_attack(victim.position) || moved;
        if (bc::fuel_remaining() > policy::observation_reserve)
            observe_fight();
    } else if (range > 1 && d > range) {
        if (d <= ready_reach && combat(true))
            return true;
        moved = attack_dash(victim.position);
        moved = advance_attack(victim.position) || moved;
        if (bc::fuel_remaining() > policy::observation_reserve)
            observe_fight();
    }
    bool acted = combat(true);
    if (acted && me.is_alive() && range <= 1 && me.can_move())
        moved = follow_through() || moved;
    // Melee retains movement after attacking. Chase after a kill and reposition
    // out of splash clumps; ranged champions cannot walk after ordinary attacks.
    if (!acted && !moved && me.can_move())
        moved = advance_attack(victim.position);
    return acted || moved;
}
