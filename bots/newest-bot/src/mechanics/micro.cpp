// Champion fight micro: one-turn combo search, dashes and follow-through.
#include "../bot.h"
#include <wasm_simd128.h>

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
    int origin = 0;
};
} // namespace

bool Bot::attack_combo(bool escaping) {
    profiling::Scope scope(profiling::Stage::fight_search);
    // Search one turn's attack/cast combinations. Host queries stay outside the
    // search; equivalent area centres collapse to one set of affected enemies.
    if (enemies.empty() || enemies.size() > std::numeric_limits<std::uint64_t>::digits)
        return false;
    auto &me = bc::self();
    const auto abilities = me.abilities();
    // Nothing acts before the search ends, so readiness is read once.
    std::array<bool, 2> is_ready{};
    {
        const bool can_cast = me.can_cast();
        for (unsigned i = 0; i < is_ready.size(); ++i)
            is_ready[i] = ready(abilities[i], can_cast);
    }
    const bool attack_available = me.can_attack();
    const bool can_attack = attack_available && (!escaping || range <= 1);
    const bool can_step = !escaping && !walked && me.can_move() && range <= 1;
    int reach = can_attack ? range : 0;
    for (unsigned i = 0; i < spells.size(); ++i) {
        const auto &s = spells[i];
        if (!is_ready[i] || (walked && s.exclusive))
            continue;
        if (s.kind == bc::ability_kind::attack_reset && !attack_available)
            reach = std::max(reach, range);
        else if (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                 s.kind == bc::ability_kind::aoe_damage)
            reach = std::max(reach, int(s.range + s.radius));
    }
    if (can_step) {
        reach = std::max(reach, range + int(stats.base_move_speed));
        for (unsigned i = 0; i < spells.size(); ++i)
            if (is_ready[i] && !spells[i].exclusive)
                reach = std::max(reach, int(spells[i].range + stats.base_move_speed));
    }
    if (std::none_of(enemies.begin(), enemies.end(),
                     [&](const auto &e) { return e.health > 0 && distance(position, e.position) <= reach; }))
        return false;
    const auto buffs = me.buffs();
    const float initial_shield = buffs.has_shield() ? buffs.get<bc::buff_shield>().value : 0;
    const bool initially_enhanced = buffs.has_enhanced_attack();
    // Working storage kept between searches: a vector allocated afresh costs
    // about 200 fuel, and growing one costs that again each time it doubles.
    static std::vector<vec2> origins;
    static std::vector<float> pressure, original, remaining, weights, follow_up, ready_follow_up, control;
    static std::vector<std::int8_t> sees;
    static std::vector<Strike> actions;
    static std::vector<int> basics, casts;
    // The enemies worth hitting, a bit each (there are at most 64).
    std::uint64_t useful = 0;
    for (unsigned i = 0; i < enemies.size(); ++i)
        if (enemies[i].health > 0 && !enemy_spawn_tile[tile_index(enemies[i].position)])
            useful |= std::uint64_t{1} << i;
    origins.assign(1, position);
    // Vision from each origin, in masks kept between searches.
    auto &sight = sight_pool;
    if (sight.empty())
        sight.emplace_back();
    bc::vision::visible_tiles_into(sight[0]);
    pressure.assign(1, incoming(position));
    if (can_step) {
        // The enemies a step could attack anew, with the reach it must bring
        // them within: melee, an enemy out of reach now and within it after
        // the step; ranged, the farthest ready reset or control spell able to
        // reach it. A step is a standing position if it brings any of them
        // within that reach, so without them no step is listed.
        float reset_reach = -1, control_reach = -1;
        for (unsigned i = 0; i < spells.size(); ++i) {
            const auto &spell = spells[i];
            if (!is_ready[i] || spell.exclusive)
                continue;
            if (spell.kind == bc::ability_kind::attack_reset)
                reset_reach = std::max(reset_reach, float(range));
            if (spell.kind == bc::ability_kind::cc)
                control_reach = std::max(control_reach, spell.range);
        }
        // Laid out four to a vector for the test of each step; a padding lane
        // has no reach.
        alignas(16) std::int32_t target_x[std::numeric_limits<std::uint64_t>::digits],
            target_y[std::numeric_limits<std::uint64_t>::digits];
        alignas(16) float target_reach[std::numeric_limits<std::uint64_t>::digits];
        unsigned target_count = 0;
        for (std::size_t k = 0; k < enemies.size(); ++k) {
            const auto &e = enemies[k];
            if (!(useful >> k & 1))
                continue;
            float reach_k;
            if (range <= 1)
                reach_k = distance(position, e.position) > range ? float(range) : -1.0f;
            else
                reach_k = !e.immune && (!e.stunned || e.stun_time <= 0) ? std::max(reset_reach, control_reach)
                                                                         : reset_reach;
            // No distance is negative, so a negative reach brings none within.
            if (reach_k >= 0) {
                target_x[target_count] = e.position.x;
                target_y[target_count] = e.position.y;
                target_reach[target_count++] = reach_k;
            }
        }
        for (unsigned k = target_count; k % 4; ++k) {
            target_x[k] = target_y[k] = 0;
            target_reach[k] = -1;
        }
        // Listed in a kept buffer: a fresh list costs an allocation.
        static std::vector<vec2> steps;
        std::uint32_t listed = 0;
        if (target_count) {
            blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
            const std::size_t side = 2 * std::size_t(std::max(0, int(stats.base_move_speed))) + 1;
            if (steps.size() < side * side)
                steps.resize(side * side);
            listed = step_tiles.positions_into(steps);
        }
        for (const vec2 tile : std::span(steps.data(), std::min<std::size_t>(listed, steps.size()))) {
            if (tile == position || origins.size() >= std::numeric_limits<std::uint64_t>::digits)
                continue;
            // float(distance(tile, target)) <= reach, four targets at a time.
            bool new_attack = false;
            const v128_t tx = wasm_i32x4_splat(tile.x), ty = wasm_i32x4_splat(tile.y);
            for (unsigned k = 0; k < target_count && !new_attack; k += 4) {
                const v128_t apart = wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(tx, wasm_v128_load(&target_x[k]))),
                                                    wasm_i32x4_abs(wasm_i32x4_sub(ty, wasm_v128_load(&target_y[k]))));
                new_attack = wasm_v128_any_true(
                    wasm_f32x4_le(wasm_f32x4_convert_i32x4(apart), wasm_v128_load(&target_reach[k])));
            }
            if (!new_attack)
                continue;
            origins.push_back(tile);
            if (sight.size() < origins.size())
                sight.emplace_back();
            me.visible_tiles_from_into(tile, sight[origins.size() - 1]);
            pressure.push_back(incoming(tile));
        }
    }
    const std::uint64_t every_origin = origins.size() == std::numeric_limits<std::uint64_t>::digits
                                           ? ~std::uint64_t{0}
                                           : (std::uint64_t{1} << origins.size()) - 1;
    // Whether each origin sees each enemy: read from the engine at most once.
    sees.assign(origins.size() * enemies.size(), -1);
    auto origin_sees = [&](std::size_t origin, vec2 target, int enemy) {
        if (enemy < 0)
            return sight[origin].is_set(target);
        auto &known = sees[origin * enemies.size() + enemy];
        if (known < 0)
            known = sight[origin].is_set(target);
        return known != 0;
    };
    auto in_range = [&](vec2 target, float reach, bool exclusive, bool needs_sight = true, int enemy = -1) {
        std::uint64_t mask = 0;
        const std::size_t count = exclusive ? 1 : origins.size();
        for (std::size_t i = 0; i < count; ++i)
            if (distance(origins[i], target) <= reach && (!needs_sight || origin_sees(i, target, enemy)))
                mask |= std::uint64_t{1} << i;
        return mask;
    };
    std::uint64_t eligible = 0;
    // Each written by index for every enemy below.
    original.resize(enemies.size());
    weights.resize(enemies.size());
    follow_up.resize(enemies.size());
    ready_follow_up.resize(enemies.size());
    control.resize(enemies.size());
    std::uint64_t threatening = 0; // The enemies threatening a kill, a bit each.
    // Concentrate the volley where it removes the most offensive power per
    // health point. Normalize within the visible fight so ordinary damage,
    // control and healing remain on comparable scales across army sizes.
    float total_threat = 0, total_health = 0;
    for (const auto &e : enemies)
        if (useful >> (&e - enemies.data()) & 1) {
            total_threat += e.damage_rate + e.burst * 0.5f;
            total_health += e.health + e.shield + e.ready_shield;
        }
    const float typical_density = total_threat / std::max(1.0f, total_health);
    for (std::size_t i = 0; i < enemies.size(); ++i) {
        const auto &e = enemies[i];
        original[i] = e.health + e.shield;
        float weight = (1 + std::min(2.0f, e.gems / 100.0f)) * (1 + 0.6f * (1 - e.health / e.max_health));
        const float density =
            (e.damage_rate + e.burst * 0.5f) / std::max(1.0f, e.health + e.shield + e.ready_shield);
        weight *= std::sqrt(density / std::max(0.001f, typical_density));
        weights[i] = weight;
        // Ready assistance excludes allies who have finished this round: an
        // enemy turn may interleave before they act, so it cannot be promised.
        float assistance = 0, ready_assistance = 0;
        bool threatens_kill = false;
        const int reach = std::max(e.range, e.spell_reach) + e.move_speed;
        const float blow = e.strength + e.burst;
        for (const auto &ally : allies) {
            threatens_kill |= !e.stunned && distance(ally.position, e.position) <= reach &&
                              blow * ally.mitigation >= ally.health;
            if (ally.stunned || ally.channel)
                continue;
            float damage = 0;
            const int d = distance(ally.position, e.position);
            if (d <= ally.range)
                damage += ally.strength;
            if (d <= std::max(ally.range, ally.spell_reach))
                damage += ally.burst;
            assistance += damage;
            if (!ally.completed)
                ready_assistance += damage;
        }
        threatens_kill |= !e.stunned && distance(position, e.position) <= reach && blow * mitigation >= health;
        follow_up[i] = assistance * e.mitigation;
        ready_follow_up[i] = ready_assistance * e.mitigation;
        if (threatens_kill)
            threatening |= std::uint64_t{1} << i;
        control[i] = blow * (escaping ? 3 : 1);
        if (useful >> i & 1)
            eligible |= std::uint64_t{1} << i;
    }
    if (!eligible)
        return false;
    remaining.resize(enemies.size());
    actions.clear();
    basics.assign(1, -1);
    casts.assign(1, -1);
    for (std::size_t i = 0; i < enemies.size(); ++i) {
        if (!(eligible & (std::uint64_t{1} << i)))
            continue;
        const auto legal = in_range(enemies[i].position, range, false, true, int(i));
        if (!legal)
            continue;
        Strike a;
        a.target = i;
        a.origins = legal;
        a.damage = strength;
        a.hits = std::uint64_t{1} << i;
        a.hits &= eligible;
        basics.push_back(actions.size());
        actions.push_back(a);
    }
    for (int slot_index = 0; slot_index < 2; ++slot_index) {
        const auto &spell = spells[slot_index];
        const auto &ability = abilities[slot_index];
        if (!is_ready[slot_index] || (walked && spell.exclusive))
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
                    (a.origins = in_range(enemies[i].position, spell.range, spell.exclusive, true, int(i)))) {
                    a.target = i;
                    a.hits = std::uint64_t{1} << i;
                    add(a);
                }
        } else if (spell.kind == bc::ability_kind::aoe_damage) {
            static std::vector<std::pair<std::uint64_t, std::uint64_t>> seen;
            seen.clear();
            // A centre seen again changes nothing: its pattern is already
            // recorded with at least its support. Skip repeats, marked in a
            // window around us that holds every centre within reach.
            const int half = int(spell.range) + 2 * int(spell.radius) + int(stats.base_move_speed) + 1;
            const int side = 2 * half + 1;
            static std::vector<std::uint64_t> scanned;
            scanned.assign((std::size_t(side) * side + 63) / 64, 0);
            auto first_visit = [&](vec2 tile) {
                const int x = tile.x - position.x + half, y = tile.y - position.y + half;
                if (x < 0 || y < 0 || x >= side || y >= side)
                    return true; // Outside the window: scan it again, which is harmless.
                const int cell = y * side + x;
                const auto bit = std::uint64_t{1} << (cell & 63);
                if (scanned[cell >> 6] & bit)
                    return false;
                scanned[cell >> 6] |= bit;
                return true;
            };
            // The enemies' tiles apart, padded to whole vectors (the padding
            // is outside every eligible mask), for the hit test of each centre
            // four enemies at a time.
            static std::vector<std::int32_t> foe_x, foe_y;
            const std::size_t padded = (enemies.size() + 3) & ~std::size_t{3};
            foe_x.resize(padded);
            foe_y.resize(padded);
            for (std::size_t i = 0; i < padded; ++i) {
                foe_x[i] = i < enemies.size() ? enemies[i].position.x : 0;
                foe_y[i] = i < enemies.size() ? enemies[i].position.y : 0;
            }
            const v128_t area = wasm_f32x4_splat(spell.radius);
            const int radius = int(spell.radius);
            const std::size_t standing = spell.exclusive ? 1 : origins.size();
            for (const auto &e : enemies) {
                if (!in_range(e.position, spell.range + spell.radius, spell.exclusive, false))
                    continue;
                for (int dx = -radius; dx <= radius; ++dx)
                    for (int dy = -radius; dy <= radius; ++dy) {
                        const vec2 tile = e.position + vec2{dx, dy};
                        if (!bc::map::is_in_bounds(tile) || !first_visit(tile))
                            continue;
                        // Only a centre that hits someone, from an origin in
                        // range and on open ground, becomes an action.
                        // distance(tile, enemy) <= radius, as a float test.
                        std::uint64_t hits = 0;
                        const v128_t tx = wasm_i32x4_splat(tile.x), ty = wasm_i32x4_splat(tile.y);
                        for (std::size_t i = 0; i < padded; i += 4) {
                            const v128_t apart =
                                wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(tx, wasm_v128_load(&foe_x[i]))),
                                               wasm_i32x4_abs(wasm_i32x4_sub(ty, wasm_v128_load(&foe_y[i]))));
                            hits |= std::uint64_t(wasm_i32x4_bitmask(
                                        wasm_f32x4_le(wasm_f32x4_convert_i32x4(apart), area)))
                                    << i;
                        }
                        hits &= eligible;
                        if (!hits)
                            continue;
                        std::uint64_t from = 0;
                        for (std::size_t i = 0; i < standing; ++i)
                            if (distance(origins[i], tile) <= spell.range)
                                from |= std::uint64_t{1} << i;
                        if (!from || no_centre_tile[tile_index(tile)])
                            continue;
                        a.tile = tile;
                        a.hits = hits;
                        a.origins = from;
                        const auto pattern = std::pair{a.hits, a.origins};
                        if (std::find(seen.begin(), seen.end(), pattern) == seen.end()) {
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
                    a.support = ability.heal_amount(allies[i].champion()) * 0.9f;
                    if (allies[i].health < allies[i].max_health * 0.5f)
                        a.support *= 2;
                    add(a);
                }
        }
    }
    Sequence best;
    const float link = linked_risk();
    // Per enemy, what the evaluator reads, computed once as it would compute
    // it: plain arrays rather than the observed units.
    struct Foe {
        float mitigation, recoil, kill_score, finish_score, credit, channel_score;
        std::uint64_t siblings; // Twins whose death this one's death causes.
        bool stun_ok, channel;
    };
    static std::vector<Foe> foes;
    foes.resize(enemies.size());
    for (std::size_t i = 0; i < enemies.size(); ++i) {
        const auto &e = enemies[i];
        auto &foe = foes[i];
        const float kill = kill_value(e);
        foe.mitigation = e.mitigation;
        foe.recoil = e.retaliation() * mitigation;
        foe.kill_score = kill * weights[i];
        foe.finish_score = kill * weights[i] * policy::assist_finish_credit;
        foe.credit = control[i] + follow_up[i] * policy::stun_followup_credit;
        foe.channel_score = float(120 + e.gems * 3);
        foe.siblings = 0;
        if (e.type == bc::champion_type::cassi || e.type == bc::champion_type::issac) {
            const auto sibling =
                e.type == bc::champion_type::cassi ? bc::champion_type::issac : bc::champion_type::cassi;
            for (std::size_t j = 0; j < enemies.size(); ++j)
                if (enemies[j].type == sibling)
                    foe.siblings |= std::uint64_t{1} << j;
        }
        foe.stun_ok = (!e.stunned || e.stun_time <= 0) && !e.immune;
        foe.channel = e.channel;
    }
    // Every sequence is scored: the search always completes. A combo is
    // scored action by action from its standing tile, so the state after its
    // first action (and, for resets, its first two) is kept per standing tile
    // and copied for every combo that starts the same way: each score comes
    // from the same operations in the same order as scoring it from scratch.
    struct ComboState {
        float score = 0, own_health = 0, own_shield = 0;
        std::uint64_t stunned = 0, touched = 0;
        bool enhanced = false, alive = false; // alive: false once the combo cannot go on.
    };
    const std::size_t foe_count = enemies.size();
    // The combo's start from a standing tile.
    const auto begin_combo = [&](ComboState &s, float *left, int origin) {
        std::copy(original.begin(), original.end(), left);
        s = {};
        if (origin && pressure[origin] * link >= health + initial_shield)
            return;
        s.score = origin ? -policy::unused_cast_cost - std::max(0.0f, pressure[origin] - pressure[0]) * link : 0;
        s.own_health = health;
        s.own_shield = initial_shield;
        s.enhanced = initially_enhanced;
        s.alive = true;
    };
    // One more action of the combo.
    const auto apply = [&](ComboState &s, float *left, int index, int origin) {
        const auto &a = actions[index];
        if (s.own_health <= 0 || (a.target >= 0 && !a.heal && left[a.target] <= 0)) {
            // A unit-targeted action cannot follow its target's death.
            s.alive = false;
            return;
        }
        if (a.reset) {
            s.enhanced = a.enhanced;
            s.score -= policy::unused_cast_cost;
            return;
        }
        if (a.shield > 0 && origin) {
            float support = std::min(a.shield, pressure[origin] * std::max(1, spells[a.slot].duration)) * 0.85f;
            if (health < pressure[origin] * 4)
                support *= 2;
            if (initial_shield > 0)
                support *= 0.2f;
            s.score += support;
        } else
            s.score += a.support;
        if (a.slot >= 0)
            s.score -= policy::unused_cast_cost; // Prefer saving a cast if it adds no value.
        s.own_shield = std::max(s.own_shield, a.shield);
        float recoil = 0;
        s.touched |= a.hits;
        // Only the enemies this action hits, in index order.
        for (auto hits = a.hits; hits; hits &= hits - 1) {
            const std::size_t i = std::countr_zero(hits);
            if (left[i] <= 0)
                continue;
            const auto &foe = foes[i];
            const float damage = a.damage * foe.mitigation;
            s.score += std::min(damage, left[i]) * weights[i];
            left[i] -= damage;
            if (damage > 0)
                recoil += foe.recoil;
            if (left[i] <= 0) {
                s.score += foe.kill_score;
                for (auto linked = foe.siblings; linked; linked &= linked - 1)
                    left[std::countr_zero(linked)] = 0; // Linked death is not a second kill reward.
                if (passive == bc::passive_type::reset_cooldowns_on_kill)
                    s.score += 60;
            } else if ((a.stun || (a.slot < 0 && s.enhanced && int(i) == a.target)) && foe.stun_ok)
                s.stunned |= std::uint64_t{1} << i;
        }
        if (a.slot < 0)
            s.enhanced = false;
        const float absorbed = std::min(s.own_shield, recoil);
        s.own_shield -= absorbed;
        s.own_health -= recoil - absorbed;
        s.score -= recoil * (1 + std::min(2.0f, gems / 100.0f)) * link;
        if (s.own_health <= 0)
            s.score -= (200 + gems * 7 + strength * 3) * link;
    };
    // The states after a first action, per standing tile and action, made on
    // first use in this search (marked with its serial), and the last state
    // after two actions.
    static std::vector<ComboState> after_one;
    static std::vector<float> after_one_left, after_two_left;
    static std::vector<std::uint32_t> after_one_mark;
    static std::uint32_t search_serial = 0;
    if (++search_serial == 0) {
        std::fill(after_one_mark.begin(), after_one_mark.end(), 0);
        search_serial = 1;
    }
    const std::size_t prefixes = origins.size() * actions.size();
    if (after_one.size() < prefixes) {
        after_one.resize(prefixes);
        after_one_mark.resize(prefixes, 0);
    }
    if (after_one_left.size() < prefixes * foe_count)
        after_one_left.resize(prefixes * foe_count);
    after_two_left.resize(foe_count);
    ComboState after_two;
    int two_origin = -1, two_first = -1, two_second = -1;
    const auto first_action = [&](int origin, int index) {
        const std::size_t key = std::size_t(origin) * actions.size() + std::size_t(index);
        if (after_one_mark[key] != search_serial) {
            after_one_mark[key] = search_serial;
            auto &s = after_one[key];
            float *left = &after_one_left[key * foe_count];
            begin_combo(s, left, origin);
            if (s.alive)
                apply(s, left, index, origin);
        }
        return key;
    };
    auto evaluate = [&](std::array<int, 3> sequence) {
        std::uint64_t legal = every_origin;
        bool restored = false;
        int listed[3], count = 0;
        for (int index : sequence)
            if (index >= 0) {
                listed[count++] = index;
                legal &= actions[index].origins;
                if (actions[index].reset)
                    restored = true;
                // Ordinary movement consumes a ranged basic allowance. Only
                // an actual reset earlier in this sequence restores that shot.
                if (range > 1 && actions[index].slot < 0 && !restored)
                    legal &= 1;
            }
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
        ComboState s;
        float *left = remaining.data();
        if (count == 0)
            begin_combo(s, left, origin);
        else if (count == 3) {
            if (two_origin != origin || two_first != listed[0] || two_second != listed[1]) {
                two_origin = origin;
                two_first = listed[0];
                two_second = listed[1];
                const std::size_t key = first_action(origin, listed[0]);
                after_two = after_one[key];
                std::copy_n(&after_one_left[key * foe_count], foe_count, after_two_left.data());
                if (after_two.alive)
                    apply(after_two, after_two_left.data(), listed[1], origin);
            }
            s = after_two;
            std::copy_n(after_two_left.data(), foe_count, left);
            if (s.alive)
                apply(s, left, listed[2], origin);
        } else {
            const std::size_t key = first_action(origin, listed[0]);
            s = after_one[key];
            std::copy_n(&after_one_left[key * foe_count], foe_count, left);
            if (count == 2 && s.alive)
                apply(s, left, listed[1], origin);
        }
        if (!s.alive)
            return;
        float score = s.score;
        const std::uint64_t stunned = s.stunned;
        // Stun only earns credit if the opponent survives the whole combo and
        // the stun opens a window someone uses: an interruption, a member it
        // saves, or follow-up damage from an ally who can still act. Enemies
        // no action hit are neither stunned nor damaged, so they are skipped.
        for (auto unscored = s.touched; unscored; unscored &= unscored - 1) {
            const std::size_t i = std::countr_zero(unscored);
            const auto &foe = foes[i];
            if ((stunned & (std::uint64_t{1} << i)) && left[i] > 0) {
                // Walking to stun spends the movement allowance, so it needs
                // a concrete payoff before the enemy's next turn.
                const bool walked_for_it = origin && !foe.channel && !(threatening >> i & 1) && ready_follow_up[i] <= 0;
                if (!walked_for_it)
                    score += foe.credit;
                if (foe.channel)
                    score += foe.channel_score;
            }
            if (left[i] > 0 && left[i] < original[i] && left[i] <= follow_up[i])
                score += foe.finish_score;
        }
        if (score > best.score)
            best = {sequence, score, origin};
    };
    // Up to four combos scored at once, one per vector lane: each lane does
    // what scoring its combo alone does, operation for operation and in the
    // same order (an operation that does not apply to a lane leaves its values
    // as they are), and the scores are then offered in the usual order. Used
    // for an attack and a run of casts, in both orders, from the tile we stand
    // on; a combo legal only from a step is scored on its own in its place.
    constexpr std::size_t batch_foes = 16;
    const bool batchable = foe_count <= batch_foes;
    struct Batch {
        v128_t score, own_health, own_shield;
        v128_t left[batch_foes], touched[batch_foes], stunned[batch_foes];
    };
    const v128_t zero = wasm_f32x4_splat(0.0f), every_lane = wasm_i32x4_splat(-1), no_lane = wasm_i32x4_splat(0);
    // The lanes whose bit is set in `bits`, from a table.
    alignas(16) static constexpr std::int32_t lane_masks[16][4] = {
        {0, 0, 0, 0},
        {-1, 0, 0, 0},
        {0, -1, 0, 0},
        {-1, -1, 0, 0},
        {0, 0, -1, 0},
        {-1, 0, -1, 0},
        {0, -1, -1, 0},
        {-1, -1, -1, 0},
        {0, 0, 0, -1},
        {-1, 0, 0, -1},
        {0, -1, 0, -1},
        {-1, -1, 0, -1},
        {0, 0, -1, -1},
        {-1, 0, -1, -1},
        {0, -1, -1, -1},
        {-1, -1, -1, -1}};
    const auto lane_mask = [](std::uint32_t bits) { return wasm_v128_load(lane_masks[bits & 15]); };
    const auto choose = [](v128_t yes, v128_t no, v128_t when) { return wasm_v128_bitselect(yes, no, when); };
    // The hit loop of apply() for one action per lane: `hit_lanes(i)` the lanes
    // whose action hits enemy i, `stun_lanes(i)` those whose hit there stuns.
    const auto strike = [&](Batch &b, v128_t live, v128_t damage, std::uint64_t any_hits, const auto &hit_lanes,
                            const auto &stun_lanes, v128_t &recoil) {
        for (auto unhit = any_hits; unhit; unhit &= unhit - 1) {
            const std::size_t i = std::countr_zero(unhit);
            const auto &foe = foes[i];
            const v128_t hit = hit_lanes(i);
            b.touched[i] = wasm_v128_or(b.touched[i], hit);
            const v128_t left = b.left[i];
            const v128_t active = wasm_v128_andnot(wasm_v128_and(hit, live), wasm_f32x4_le(left, zero));
            const v128_t dealt = wasm_f32x4_mul(damage, wasm_f32x4_splat(foe.mitigation));
            b.score = choose(wasm_f32x4_add(b.score, wasm_f32x4_mul(wasm_f32x4_pmin(dealt, left),
                                                                     wasm_f32x4_splat(weights[i]))),
                             b.score, active);
            const v128_t after = wasm_f32x4_sub(left, dealt);
            b.left[i] = choose(after, left, active);
            recoil = choose(wasm_f32x4_add(recoil, wasm_f32x4_splat(foe.recoil)), recoil,
                            wasm_v128_and(active, wasm_f32x4_gt(dealt, zero)));
            const v128_t killed = wasm_v128_and(active, wasm_f32x4_le(after, zero));
            b.score = choose(wasm_f32x4_add(b.score, wasm_f32x4_splat(foe.kill_score)), b.score, killed);
            for (auto linked = foe.siblings; linked; linked &= linked - 1) {
                const std::size_t twin = std::countr_zero(linked);
                b.left[twin] = choose(zero, b.left[twin], killed); // Linked death is not a second kill reward.
            }
            if (passive == bc::passive_type::reset_cooldowns_on_kill)
                b.score = choose(wasm_f32x4_add(b.score, wasm_f32x4_splat(60.0f)), b.score, killed);
            if (foe.stun_ok)
                b.stunned[i] = wasm_v128_or(b.stunned[i], wasm_v128_andnot(wasm_v128_and(active, stun_lanes(i)), killed));
        }
    };
    // What apply() does after the hit loop (the action is not an attack's,
    // or `enhanced` is not read again).
    const auto settle = [&](Batch &b, v128_t recoil) {
        const v128_t absorbed = wasm_f32x4_pmin(b.own_shield, recoil);
        b.own_shield = wasm_f32x4_sub(b.own_shield, absorbed);
        b.own_health = wasm_f32x4_sub(b.own_health, wasm_f32x4_sub(recoil, absorbed));
        b.score = wasm_f32x4_sub(b.score, wasm_f32x4_mul(wasm_f32x4_mul(recoil, wasm_f32x4_splat(1 + std::min(2.0f, gems / 100.0f))),
                                                          wasm_f32x4_splat(link)));
        b.score = choose(wasm_f32x4_sub(b.score, wasm_f32x4_splat((200 + gems * 7 + strength * 3) * link)), b.score,
                         wasm_f32x4_le(b.own_health, zero));
    };
    // The credits evaluate() adds after the actions, standing on our own tile.
    const auto credit = [&](Batch &b) {
        for (std::size_t i = 0; i < foe_count; ++i) {
            if (!wasm_v128_any_true(b.touched[i]))
                continue;
            const auto &foe = foes[i];
            const v128_t left = b.left[i];
            const v128_t standing = wasm_v128_and(b.touched[i], wasm_f32x4_gt(left, zero));
            const v128_t stunned = wasm_v128_and(b.stunned[i], standing);
            b.score = choose(wasm_f32x4_add(b.score, wasm_f32x4_splat(foe.credit)), b.score, stunned);
            if (foe.channel)
                b.score = choose(wasm_f32x4_add(b.score, wasm_f32x4_splat(foe.channel_score)), b.score, stunned);
            const v128_t finishing =
                wasm_v128_and(wasm_v128_and(standing, wasm_f32x4_lt(left, wasm_f32x4_splat(original[i]))),
                              wasm_f32x4_le(left, wasm_f32x4_splat(follow_up[i])));
            b.score = choose(wasm_f32x4_add(b.score, wasm_f32x4_splat(foe.finish_score)), b.score, finishing);
        }
    };
    // Copies a batch's state for the enemies there are.
    const auto copy_batch = [&](Batch &to, const Batch &from) {
        to.score = from.score;
        to.own_health = from.own_health;
        to.own_shield = from.own_shield;
        for (std::size_t i = 0; i < foe_count; ++i) {
            to.left[i] = from.left[i];
            to.touched[i] = from.touched[i];
            to.stunned[i] = from.stunned[i];
        }
    };
    // Runs of up to four consecutive casts that are not resets, in the order
    // the search meets them: each lane's cast, and the state after that cast
    // alone from the start (the lanes where that combo goes on).
    struct CastRun {
        int cast[4], used;
        v128_t support, shield, damage, stuns;
        v128_t hit[batch_foes];
        std::uint64_t any_hits;
        Batch alone;
        std::uint32_t alone_live;
    };
    // Kept between searches, in static storage (each run is written in full
    // before it is read): there are at most as many runs as casts.
    constexpr std::size_t run_room = 32;
    static CastRun runs[run_room];
    // (casts lists a cast-free entry first: a run of three needs four entries.)
    const bool batched = batchable && casts.size() >= 4 && casts.size() <= run_room;
    std::size_t run_count = 0;
    // Each lane's cast applied to a batch, as apply() applies it, where `live`.
    const auto cast_lanes = [&](Batch &b, const CastRun &run, std::uint32_t live) {
        // A cast: its support (from our own tile), the unused cast, the shield.
        b.score = wasm_f32x4_add(b.score, run.support);
        b.score = wasm_f32x4_sub(b.score, wasm_f32x4_splat(policy::unused_cast_cost));
        b.own_shield = wasm_f32x4_pmax(b.own_shield, run.shield);
        v128_t recoil = zero;
        strike(
            b, lane_mask(live), run.damage, run.any_hits, [&](std::size_t i) { return run.hit[i]; },
            [&](std::size_t) { return run.stuns; }, recoil);
        settle(b, recoil);
    };
    // The lanes whose cast passes apply()'s first check on a state the same
    // for every lane.
    const auto casts_go_on = [&](const CastRun &run, bool alive, float own, const float *left) {
        std::uint32_t live = 0;
        for (int j = 0; j < run.used; ++j) {
            const auto &a = actions[run.cast[j]];
            if (alive && !(own <= 0) && !(a.target >= 0 && !a.heal && left[a.target] <= 0))
                live |= 1u << j;
        }
        return live;
    };
    if (batched) {
        for (std::size_t c = 0; c < casts.size();) {
            if (casts[c] < 0 || actions[casts[c]].reset) {
                ++c;
                continue;
            }
            auto &run = runs[run_count++];
            run.used = 0;
            alignas(16) float support[4] = {}, shield[4] = {}, damage[4] = {};
            std::uint64_t hits[4] = {};
            std::uint32_t stuns = 0;
            while (run.used < 4 && c < casts.size() && casts[c] >= 0 && !actions[casts[c]].reset) {
                const int j = run.used++;
                const auto &a = actions[casts[c]];
                run.cast[j] = casts[c++];
                support[j] = a.support;
                shield[j] = a.shield;
                damage[j] = a.damage;
                hits[j] = a.hits;
                if (a.stun)
                    stuns |= 1u << j;
            }
            // Fewer than three casts cost less scored one combo at a time.
            if (run.used < 3)
                continue;
            run.support = wasm_v128_load(support);
            run.shield = wasm_v128_load(shield);
            run.damage = wasm_v128_load(damage);
            run.stuns = lane_mask(stuns);
            run.any_hits = hits[0] | hits[1] | hits[2] | hits[3];
            for (std::size_t i = 0; i < foe_count; ++i)
                run.hit[i] = lane_mask(std::uint32_t(hits[0] >> i & 1) | std::uint32_t(hits[1] >> i & 1) << 1 |
                                       std::uint32_t(hits[2] >> i & 1) << 2 | std::uint32_t(hits[3] >> i & 1) << 3);
            // The start (our own tile: begin_combo()), then each cast.
            Batch &b = run.alone;
            b.score = zero;
            b.own_health = wasm_f32x4_splat(health);
            b.own_shield = wasm_f32x4_splat(initial_shield);
            for (std::size_t i = 0; i < foe_count; ++i) {
                b.left[i] = wasm_f32x4_splat(original[i]);
                b.touched[i] = b.stunned[i] = no_lane;
            }
            run.alone_live = casts_go_on(run, true, health, original.data());
            cast_lanes(b, run, run.alone_live);
        }
    }
    // Evaluate attacks alone first, then both cast orders. Resets get a second
    // independently selected attack, allowing a kill followed by a new target.
    for (int first : basics) {
        if (first >= 0 && !can_attack)
            continue;
        evaluate({first, -1, -1});
        std::size_t next_run = 0;
        for (std::size_t c = 0; c < casts.size();) {
            const int cast = casts[c];
            if (cast < 0) {
                ++c;
                continue;
            }
            if (actions[cast].reset) {
                ++c;
                if (first < 0 && attack_available && !(can_step && range > 1))
                    continue;
                for (int second : basics)
                    if (second >= 0)
                        evaluate({first, cast, second});
                continue;
            }
            if (!batched) {
                ++c;
                evaluate({first, cast, -1});
                // Without an attack both orders are the cast alone, already scored.
                if (first >= 0)
                    evaluate({cast, first, -1});
                continue;
            }
            // The run of casts starting here, scored together, offered in order.
            const CastRun &run = runs[next_run++];
            c += std::size_t(run.used);
            if (run.used < 3) {
                for (int j = 0; j < run.used; ++j) {
                    evaluate({first, run.cast[j], -1});
                    if (first >= 0)
                        evaluate({run.cast[j], first, -1});
                }
                continue;
            }
            std::uint32_t legal = 0;
            const std::uint64_t standing = first >= 0 ? actions[first].origins : every_origin;
            for (int j = 0; j < run.used; ++j)
                if (standing & actions[run.cast[j]].origins & 1)
                    legal |= 1u << j;
            alignas(16) float then_cast[4], cast_first[4];
            std::uint32_t cast_scored = 0, attack_scored = 0;
            Batch b;
            if (first < 0) {
                // The cast alone.
                cast_scored = run.alone_live & legal;
                if (cast_scored) {
                    copy_batch(b, run.alone);
                    credit(b);
                    wasm_v128_store(then_cast, b.score);
                }
            } else {
                // The attack, then each lane's cast.
                const std::size_t key = first_action(0, first);
                const ComboState &s = after_one[key];
                const float *left = &after_one_left[key * foe_count];
                cast_scored = casts_go_on(run, s.alive, s.own_health, left) & legal;
                if (cast_scored) {
                    b.score = wasm_f32x4_splat(s.score);
                    b.own_health = wasm_f32x4_splat(s.own_health);
                    b.own_shield = wasm_f32x4_splat(s.own_shield);
                    for (std::size_t i = 0; i < foe_count; ++i) {
                        b.left[i] = wasm_f32x4_splat(left[i]);
                        b.touched[i] = s.touched >> i & 1 ? every_lane : no_lane;
                        b.stunned[i] = s.stunned >> i & 1 ? every_lane : no_lane;
                    }
                    cast_lanes(b, run, cast_scored);
                    credit(b);
                    wasm_v128_store(then_cast, b.score);
                }
                // Each lane's cast, then the attack.
                const auto &a = actions[first];
                if (run.alone_live & legal) {
                    copy_batch(b, run.alone);
                    v128_t going_on = wasm_v128_andnot(lane_mask(run.alone_live & legal), wasm_f32x4_le(b.own_health, zero));
                    if (a.target >= 0 && !a.heal)
                        going_on = wasm_v128_andnot(going_on, wasm_f32x4_le(b.left[a.target], zero));
                    attack_scored = std::uint32_t(wasm_i32x4_bitmask(going_on));
                    if (attack_scored) {
                        // An attack: its support (from our own tile) and shield.
                        b.score = wasm_f32x4_add(b.score, wasm_f32x4_splat(a.support));
                        b.own_shield = wasm_f32x4_pmax(b.own_shield, wasm_f32x4_splat(a.shield));
                        v128_t recoil = zero;
                        // Casts leave `enhanced` as it starts.
                        const v128_t stunning = a.stun || initially_enhanced ? every_lane : no_lane;
                        strike(
                            b, going_on, wasm_f32x4_splat(a.damage), a.hits, [&](std::size_t) { return every_lane; },
                            [&](std::size_t i) { return a.stun || int(i) == a.target ? stunning : no_lane; }, recoil);
                        settle(b, recoil);
                        credit(b);
                        wasm_v128_store(cast_first, b.score);
                    }
                }
            }
            for (int j = 0; j < run.used; ++j) {
                if (!(legal >> j & 1) && origins.size() > 1) {
                    // Not from this tile but maybe from a step: scored alone, here.
                    evaluate({first, run.cast[j], -1});
                    if (first >= 0)
                        evaluate({run.cast[j], first, -1});
                    continue;
                }
                if ((cast_scored >> j & 1) && then_cast[j] > best.score)
                    best = {{first, run.cast[j], -1}, then_cast[j], 0};
                if ((attack_scored >> j & 1) && cast_first[j] > best.score)
                    best = {{run.cast[j], first, -1}, cast_first[j], 0};
            }
        }
    }
    if (best.score <= 0)
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
            const auto &enemy = enemies[a.target].champion();
            if (me.can_attack() && enemy.is_visible() && enemy.is_alive()) {
                if (me.attack_tiles().is_set(enemy.position())) {
                    me.attack(enemy);
                    acted = true;
                }
            }
        } else if (ready(abilities[a.slot], me.can_cast())) {
            bool used = false;
            if (a.reset && me.can_attack())
                continue;
            if (a.reset || a.shield > 0)
                used = abilities[a.slot].use();
            else if (a.heal)
                used = abilities[a.slot].use(allies[a.target].champion());
            else if (spells[a.slot].kind == bc::ability_kind::aoe_damage)
                used = abilities[a.slot].use(a.tile);
            else {
                const auto &enemy = enemies[a.target].champion();
                if (enemy.is_visible() && enemy.is_alive())
                    used = abilities[a.slot].use(enemy);
            }
            acted |= used;
        }
    }
    refresh_self();
    return acted;
}

bool Bot::attack_dash(vec2 victim) {
    auto &me = bc::self();
    if (!me.can_attack())
        return false;
    const int gap = distance(position, victim) - range;
    if (gap <= 0 || (range <= 1 && gap <= int(stats.base_move_speed)))
        return false; // Walking into melee preserves the cast for an attack reset.
    const bool can_cast = me.can_cast();
    for (const auto &ability : me.abilities()) {
        if (!ability.is_dash() || !ready(ability, can_cast))
            continue;
        const auto design = describe(ability);
        if (gap > design.range || (walked && design.exclusive))
            continue;
        occupied_into(dash_blocked, dash_terrain);
        dash_blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(unsigned(design.range)),
                                          step_tiles);
        subtract_into(search_goals, step_tiles, blocked);
        vec2 best = position;
        float best_score = std::numeric_limits<float>::infinity();
        for (auto tile : search_goals.positions()) {
            if (tile == position || distance(tile, victim) > range)
                continue;
            if (!(float(distance(tile, victim)) < best_score))
                continue; // The incoming estimate only adds: this tile cannot win.
            float score = incoming(tile) * 0.14f + float(distance(tile, victim));
            if (score < best_score) {
                best_score = score;
                best = tile;
            }
        }
        if (best != position && ability.use(best)) {
            refresh_self();
            walked |= design.exclusive;
            refresh_blocked();
            return true;
        }
    }
    return false;
}

bool Bot::follow_through() {
    // Melee attacks leave a move available. Use it to line up the next attack
    // or reach the next victim immediately after a kill.
    for (auto &e : enemies) {
        const auto info = sighting(e.champion());
        e.health = info ? info->health : 0;
        if (e.health > 0)
            e.shield = live_shield(e);
    }
    refresh_blocked();
    auto value = [&](vec2 tile) {
        float best = 0;
        for (const auto &e : enemies) {
            if (e.health <= 0 || enemy_spawn_tile[tile_index(e.position)])
                continue;
            const int arrival = std::max(0, distance(tile, e.position) - range);
            float damage = std::min(strength * e.mitigation, e.health + e.shield);
            if (e.health + e.shield <= strength * e.mitigation)
                damage += 200 + e.gems * 7;
            best = std::max(best, damage / (1 + arrival));
        }
        return best;
    };
    vec2 best = position;
    float best_score = value(position);
    blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
    for (auto tile : step_tiles.positions()) {
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
    if (!me.can_move() || distance(position, victim) <= range)
        return false;
    // A visible fight needs a local approach, not a route across the whole map.
    // Reserve the remaining turn for the attack and reset after this move.
    const auto local_blocked = blocked | ~around(position, stats.vision_radius);
    const auto goals = around(victim, range) & ~local_blocked;
    if (goals.is_empty())
        return false;
    auto &field = move_field;
    field.compute_to(local_blocked, goals, origin_at(position), search_visited);
    if (field.get(position) == bc::distance_field::unreachable)
        return false;
    const bool moved = cast_mobility(field, false, -1);
    if (moved)
        refresh_blocked();
    if (!me.can_move() || distance(position, victim) <= range)
        return moved;
    vec2 best = position;
    float best_score = std::numeric_limits<float>::infinity();
    blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
    for (auto tile : step_tiles.positions()) {
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
    refresh_blocked();
    // Built in storage kept between fights, then swapped in.
    auto &current = fight_scratch;
    current.clear();
    for (const auto &c : enemy_roster) {
        const auto info = sighting(c);
        if (!info)
            continue; // A champion in view is alive.
        const auto found =
            std::find_if(enemies.begin(), enemies.end(), [&](const auto &e) { return e.champion() == c; });
        if (found == enemies.end())
            current.emplace_back(c, *info);
        else {
            current.push_back(*found);
            auto &e = current.back();
            e.position = {info->x, info->y};
            e.health = info->health;
            e.shield = 0;
            read_buffs(e);
        }
    }
    enemies.swap(current);
    ++threat_revision; // Their nearest units moved.
}

bool Bot::impact_dash(bool fighting_champions, int farm_group) {
    // Heart of Ruin's dash lands up to two tiles away and hits everything
    // within one tile of the landing. Choose the landing by what it hits.
    auto &me = bc::self();
    if (walked || !me.can_cast())
        return false;
    auto abilities = me.abilities();
    for (unsigned slot_index = 0; slot_index < spells.size(); ++slot_index) {
        const auto &spell = spells[slot_index];
        auto &ability = abilities[slot_index];
        if (spell.kind != bc::ability_kind::dash || spell.modifier <= 0 || spell.radius <= 0 ||
            !ready(ability, me.can_cast()))
            continue;
        const float damage = ability.damage_amount();
        const std::span<const ObservedUnit> targets =
            fighting_champions ? std::span<const ObservedUnit>(enemies) : camps.group(farm_group);
        // The units a landing may hit, in order, and what a hit on each adds
        // (a hit's damage, then for a champion its kill or disruption value):
        // none of it depends on the landing (enemies in their spawn, and
        // camps of other groups, are never hit). A landing is within the
        // dash's steps of here, so a unit further than those and the radius
        // is never hit; with none nearer, no landing scores.
        const int steps = static_cast<std::uint16_t>(unsigned(spell.range));
        struct Hittable {
            vec2 position;
            float dealt, extra;
        };
        static std::vector<Hittable> hittable;
        hittable.clear();
        for (const auto &unit : targets)
            if (unit.health > 0 && float(distance(position, unit.position) - steps) <= spell.radius &&
                !(fighting_champions ? enemy_spawn_tile[tile_index(unit.position)]
                                     : unit.group != farm_group)) {
                const float multiplier = unit.enemy() ? unit.mitigation : camp_multiplier;
                const float dealt = std::min(damage * multiplier, unit.health + unit.shield);
                float extra = 0;
                if (fighting_champions) {
                    if (dealt >= unit.health + unit.shield)
                        extra = kill_value(unit);
                    else if (!unit.immune && !(unit.stunned && unit.stun_time > 0))
                        extra = unit.strength + unit.burst + (unit.channel ? 120 + unit.gems * 3 : 0);
                }
                hittable.push_back({unit.position, dealt, extra});
            }
        if (hittable.empty())
            continue;
        occupied_into(dash_blocked, dash_terrain);
        dash_blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(steps), step_tiles);
        subtract_into(search_goals, step_tiles, blocked);
        // Landings within the dash's range on both axes, listed into storage
        // kept between turns.
        const std::size_t span = 2 * std::size_t(steps) + 1;
        if (dash_landings.size() < span * span)
            dash_landings.resize(span * span);
        const std::uint32_t count = search_goals.positions_into(dash_landings);
        vec2 best{-1, -1};
        float best_score = 0;
        for (std::uint32_t k = 0; k < count; ++k) {
            const vec2 tile = dash_landings[k];
            if (tile == position || enemy_spawn_tile[tile_index(tile)])
                continue;
            float score = 0;
            int hits = 0;
            for (const auto &hit : hittable) {
                if (distance(tile, hit.position) > spell.radius)
                    continue;
                score += hit.dealt;
                ++hits;
                if (fighting_champions)
                    score += hit.extra;
            }
            if (!hits || !(score > best_score))
                continue; // Danger only lowers the score: this landing cannot win.
            // A bruiser wants to be in the fight; only refuse a landing it
            // would not survive.
            const float danger = incoming(tile, farm_group);
            if (danger >= health)
                continue;
            score -= danger * 0.2f;
            if (score > best_score) {
                best_score = score;
                best = tile;
            }
        }
        // On camps, spend the dash only when it does real work.
        if (best.x < 0 || (!fighting_champions && best_score < damage * 0.5f))
            continue;
        if (ability.use(best)) {
            walked |= spell.exclusive;
            refresh_self();
            refresh_blocked();
            if (farm_group >= 0)
                groups[farm_group].attacked = round;
            return true;
        }
    }
    return false;
}
