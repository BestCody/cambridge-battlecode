// Single-turn attack and cast selection, plus kill and linked-fate values.
#include "../bot.h"

namespace {
bool present(const ObservedUnit &u) { return u.unit().is_visible() && u.unit().health() > 0; }
} // namespace

bool Bot::combat(bool fighting_champions, int farm_group, bool escaping) {
    if (bc::fuel_remaining() < (squad_member() ? policy::planning_reserve : policy::action_reserve))
        return false;
    auto &me = bc::self();
    if (me.is_teleporting() || me.is_recalling() || me.is_submitting_gems())
        return false;
    if (bc::fuel_remaining() < policy::action_reserve)
        return false;
    if (!me.can_attack() && !me.can_cast())
        return false;
    // A damaging dash (Heart of Ruin) engages first; the rest of the turn
    // continues from the landing tile.
    const bool dashed = !escaping && impact_dash(fighting_champions, farm_group);
    if (fighting_champions && attack_combo(escaping))
        return true;
    if (bc::fuel_remaining() < policy::action_reserve)
        return false;
    auto &targets = fighting_champions ? enemies : camps;
    const auto enemy_spawn = bc::map::spawn_region(bc::team::enemy);
    const auto forbidden_centres = bc::map::walls() | bc::map::holes() | bc::map::altars();
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
    std::vector<bool> relevant(targets.size(), true);
    std::vector<float> controls(targets.size());
    for (unsigned i = 0; i < targets.size(); ++i) {
        relevant[i] = !squad_member() || !fighting_champions || relevant_enemy(targets[i]);
        controls[i] = squad_member() && fighting_champions
                          ? control_value(targets[i])
                          : (targets[i].strength + targets[i].burst) * (escaping ? 3 : 1);
    }
    auto eligible = [&](const ObservedUnit &u) {
        return (fighting_champions || u.group == farm_group) && u.health > 0 &&
               relevant[&u - targets.data()] && !(fighting_champions && enemy_spawn.is_set(u.position));
    };
    auto weight = [&](const ObservedUnit &u, float damage, bool cc) {
        if (!eligible(u))
            return 0.0f;
        float effective = damage * (u.enemy() ? u.mitigation : camp_multiplier);
        float value = std::min(effective, u.health + u.shield);
        if (effective >= u.health + u.shield) {
            value += u.enemy() ? kill_value(u) : u.strength * 8 + 20;
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
            value += controls[&u - targets.data()];
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
                  distance(struck, t.position) <= bc::constants::passive_abilities::varg_splash_radius))) {
                const float before = t.health;
                t.health = t.unit().is_visible() ? t.unit().health() : 0;
                if (!fighting_champions)
                    record_camp_damage(t.group, std::max(0.0f, before - t.health));
            }
        refresh_self();
        own_shield = read_shield();
        return true;
    };
    // Normal attack precedes resets. Ranged champions may also move, reset and
    // attack; the reset evaluator below accounts for that restored allowance.
    if (!escaping || range <= 1)
        do_attack();
    if (!me.is_alive())
        return acted || dashed;

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
                                forbidden_centres.is_set(tile))
                                continue;
                            float score = 0;
                            for (const auto &other : targets)
                                if (distance(tile, other.position) <= s.radius)
                                    score += weight(other, damage, s.duration > 0);
                            if (squad_member())
                                score -=
                                    collateral_risk(tile, s.radius, fighting_champions ? -1 : farm_group);
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
                                     int(i) == best_target))) {
                    const float before = u.health;
                    u.health = u.unit().is_visible() ? u.unit().health() : 0;
                    if (!fighting_champions)
                        record_camp_damage(u.group, std::max(0.0f, before - u.health));
                }
            }
            refresh_self();
            own_shield = read_shield();
            do_attack();
        }
    }
    return acted || dashed;
}

float Bot::kill_value(const ObservedUnit &enemy) const {
    float value = 200 + enemy.gems * 7 + enemy.strength * 3;
    const bool twin = enemy.type == bc::champion_type::cassi || enemy.type == bc::champion_type::issac;
    if (twin) {
        // One kill reward, plus the known combat power and bag of the sibling.
        const auto other =
            enemy.type == bc::champion_type::cassi ? bc::champion_type::issac : bc::champion_type::cassi;
        for (const auto &unit : enemies)
            if (unit.type == other && unit.health > 0)
                value += unit.strength * 3 + unit.burst + unit.gems * 7;
    }
    return value;
}

float Bot::linked_risk() const {
    if (type != bc::champion_type::cassi && type != bc::champion_type::issac)
        return 1;
    float risk = 1.5f; // Losing a second body matters even when its bag is unseen.
    const auto sibling =
        type == bc::champion_type::cassi ? bc::champion_type::issac : bc::champion_type::cassi;
    for (const auto &ally : allies)
        if (ally.type == sibling)
            risk += std::min(2.0f, ally.gems / 100.0f);
    return risk;
}
