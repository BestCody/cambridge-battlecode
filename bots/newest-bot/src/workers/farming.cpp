// Worker camp choice and farming.
#include "../bot.h"
#include <wasm_simd128.h>

namespace {
// Carries take capture rewards first: the holder gets the gold, and the top
// team spends it on these two.
bool carry(bc::champion_type t) {
    return t == bc::champion_type::stellar || t == bc::champion_type::heart_of_ruin;
}
} // namespace

int Bot::worker_choose_group(int excluded) {
    profiling::Scope scope(profiling::Stage::choose);
    measure_paths(1);
    float progress = std::min(1.0f, round / float(std::max(1u, rules::settings().max_rounds / 2)));
    float gold_value = policy::early_gold_value * (1 - progress) + policy::late_gold_value * progress;
    const float reserve = std::max(max_health * policy::casualty_margin, incoming(position) * 2);
    // Squad assembly: count living allies. After a wipe a lone respawn must not
    // walk straight back into a grouped enemy and feed one champion at a time.
    std::uint64_t alive = 0; // Bit s: roster slot s.
    int alive_allies = 0;
    for (std::size_t s = 0; s < roster.size() && s < 64; ++s)
        if (roster[s].is_alive()) {
            alive |= std::uint64_t{1} << s;
            ++alive_allies;
        }
    const auto living = [&](unsigned s) { return s < 64 && (alive >> s & 1); };
    const bool depleted = float(alive_allies) <= float(roster.size()) * policy::depleted_alive_fraction;
    // Where the rest of the team works: living teammates' claimed camps and
    // the allies in view. Camps near them keep help within reach.
    std::array<std::int32_t, TeamMemory::slots + 32> team_x, team_y;
    int team = 0;
    if constexpr (policy::cohesion_weight > 0) {
        for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
            if (s != slot && living(s)) {
                const int claimed = TeamMemory::objective(shared.get(s), groups.size());
                if (claimed >= 0) {
                    team_x[team] = groups[claimed].centre.x;
                    team_y[team++] = groups[claimed].centre.y;
                }
            }
        for (const auto &ally : allies)
            if (team < int(team_x.size())) {
                team_x[team] = ally.position.x;
                team_y[team++] = ally.position.y;
            }
    }
    // Per group, from one pass over the claims, the squad and the units in
    // view: teammates' claims (each claimant can supply the next clear's
    // damage, so each claim is also a partner), a living squad member's
    // objective, and an ally or an enemy standing on it.
    enum : std::uint8_t { reserved = 1, ally_on = 2, enemy_on = 4, carry_on = 8 };
    static std::vector<std::uint8_t> claims, marks;
    claims.assign(groups.size(), 0);
    marks.assign(groups.size(), 0);
    for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
        if (s != slot && shared.get(s) >= 1 && shared.get(s) <= groups.size())
            ++claims[shared.get(s) - 1];
    if (squad.members)
        for (unsigned member = 0; member < roster.size(); ++member)
            if (squad.contains(member) && living(member))
                if (const int objective = TeamMemory::objective(shared.get(member), groups.size());
                    objective >= 0)
                    marks[objective] |= reserved;
    const auto standing_on = [&](vec2 p) {
        const int g = group_at(p);
        return g >= 0 && distance(p, groups[g].centre) <= int(groups[g].camps.size()) ? g : -1;
    };
    for (const auto &a : allies)
        if (const int g = standing_on(a.position); g >= 0)
            marks[g] |= ally_on | (policy::single_holder && carry(a.type) ? carry_on : 0);
    // A held camp is left to its holder; a carry only leaves one a carry holds.
    const std::int32_t holder_on = policy::single_holder && carry(type) ? carry_on : ally_on;
    for (const auto &e : enemies)
        if (const int g = standing_on(e.position); g >= 0)
            marks[g] |= enemy_on;
    const int here = group_at(position);
    const float gem_weight = gem_reward_weight(opening), gem_risk = gem_risk_weight(opening);
    const int capture_time = rules::settings().capture_time;
    const float health_scale = std::max(100.0f, health), spare = std::max(0.0f, health - reserve);
    const bool issac_link = squad.members && type == bc::champion_type::issac;
    const float link = issac_link ? linked_risk() : 0.0f;
    const bool weak = health < max_health * policy::retreat_health;
    // Allies close enough to share the next clear and committed to no other
    // camp (no claim, or a claim on a finished one): every camp counts them
    // as helpers, as it counts its claimants.
    int free_partners = 0;
    if constexpr (policy::partner_radius > 0)
        for (const auto &ally : allies) {
            if (ally.health <= 0 || distance(position, ally.position) > policy::partner_radius)
                continue;
            for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
                if (s != slot && ally.champion() == roster[s]) {
                    const unsigned code = shared.get(s);
                    const int claimed = TeamMemory::objective(code, groups.size());
                    free_partners += code == 0 || (claimed >= 0 && groups[claimed].status == CampGroup::cooling);
                    break;
                }
        }
    constexpr float skipped = -std::numeric_limits<float>::infinity();

    // Each group's inputs in lanes of four, the tail padded with lanes that
    // are never chosen: what never changes, gathered once, and this choice's
    // state, gathered by plain loads. Scores come four groups per vector step,
    // every lane in the float operations of the scalar formula and in its
    // order; a condition selects between results rather than branching.
    const std::size_t count = groups.size(), padded = (count + 3) & ~std::size_t{3};
    struct Lanes {
        std::size_t padded = 0;
        // Two blocks made once and never filled whole: the fixed lanes and
        // every padding lane are written here, this choice's lanes by each
        // gather before any read.
        std::unique_ptr<std::int32_t[]> ints;
        std::unique_ptr<float[]> floats;
        // Fixed: masks for more than one camp, contested ground, a gem reward
        // and the enemy's side; the gem reward, the gold, the altar term and
        // the centre.
        std::int32_t *several, *contested, *gem, *enemy_side, *x, *y;
        float *gem_reward, *gold, *altar;
        // This choice's state.
        std::int32_t *status, *respawn, *path, *siege_open, *marks, *sighted;
        float *remaining, *splash, *pressure, *claims, *scores;
    };
    static Lanes lanes;
    if (lanes.padded != padded) {
        lanes.padded = padded;
        lanes.ints.reset(new std::int32_t[12 * padded]);
        lanes.floats.reset(new float[8 * padded]);
        std::int32_t *next_int = lanes.ints.get();
        for (auto **lane :
             {&lanes.several, &lanes.contested, &lanes.gem, &lanes.enemy_side, &lanes.x, &lanes.y,
              &lanes.status, &lanes.respawn, &lanes.path, &lanes.siege_open, &lanes.marks, &lanes.sighted}) {
            *lane = next_int;
            next_int += padded;
        }
        float *next_float = lanes.floats.get();
        for (auto **lane : {&lanes.gem_reward, &lanes.gold, &lanes.altar, &lanes.remaining, &lanes.splash,
                            &lanes.pressure, &lanes.claims, &lanes.scores}) {
            *lane = next_float;
            next_float += padded;
        }
        for (std::size_t i = count; i < padded; ++i) {
            for (auto *lane : {lanes.several, lanes.contested, lanes.gem, lanes.enemy_side, lanes.x, lanes.y,
                               lanes.status, lanes.respawn, lanes.siege_open, lanes.marks})
                lane[i] = 0;
            for (auto *lane : {lanes.gem_reward, lanes.gold, lanes.altar, lanes.remaining, lanes.splash,
                               lanes.pressure, lanes.claims})
                lane[i] = 0;
            lanes.path[i] = bc::distance_field::unreachable; // Padding is never chosen.
        }
        for (std::size_t i = 0; i < padded; ++i)
            lanes.sighted[i] = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto &g = groups[i];
            const auto gem_value = g.unit.gem_value();
            lanes.several[i] = g.camps.size() > 1 ? -1 : 0;
            lanes.contested[i] = g.enemy_distance <= g.home_distance + 8 ? -1 : 0;
            lanes.gem[i] = gem_value > 0 ? -1 : 0;
            lanes.enemy_side[i] = g.enemy_distance < g.home_distance ? -1 : 0;
            lanes.x[i] = g.centre.x;
            lanes.y[i] = g.centre.y;
            lanes.gem_reward[i] = gem_value * gem_weight;
            lanes.gold[i] = float(g.unit.gold_value());
            lanes.altar[i] = std::min(20, g.altar_distance) * 0.25f;
        }
    }
    const auto gather = [&] {
        std::int32_t *__restrict status = lanes.status, *__restrict respawn = lanes.respawn,
                                 *__restrict path = lanes.path, *__restrict siege = lanes.siege_open,
                                 *__restrict mark = lanes.marks;
        float *__restrict remaining = lanes.remaining, *__restrict splash = lanes.splash,
                          *__restrict pressure = lanes.pressure, *__restrict claim = lanes.claims;
        const int *paths = group_path.data();
        for (std::size_t i = 0; i < count; ++i) {
            const auto &g = groups[i];
            status[i] = int(g.status);
            respawn[i] = g.respawn;
            path[i] = paths[i];
            siege[i] = (range > 1 || policy::siege_melee) && g.siege_open > 0 ? -1 : 0;
            mark[i] = marks[i];
            remaining[i] = g.status == CampGroup::cooling ? g.max_health : g.health;
            splash[i] = g.splash;
            pressure[i] = g.pressure;
            claim[i] = float(claims[i]);
        }
        if constexpr (policy::worker_pure_resource)
            for (std::size_t i = 0; i < count; ++i)
                lanes.sighted[i] = i < enemy_seen_near.size() &&
                                           int(round) - enemy_seen_near[i] <= policy::worker_sighting_memory
                                       ? -1
                                       : 0;
    };
    const auto splat = [](float v) { return wasm_f32x4_splat(v); };
    const auto splat_int = [](std::int32_t v) { return wasm_i32x4_splat(v); };
    const v128_t one = splat(1.0f), zero = splat(0.0f), no_bits = splat_int(0);
    const bool splash_passive = passive == bc::passive_type::splash_attack;
    const float flat_dps = splash_passive ? 0.0f : spell_dps(spells, strength, focus, 1);
    const float retaliation = passive == bc::passive_type::retaliation
                                  ? focus * bc::constants::passive_abilities::ren_retaliation_focus_ratio
                                  : 0.0f;
    const float contest_factor = depleted ? policy::depleted_penalty : (1 + policy::contest_bonus);
    const auto pick = [](v128_t yes, v128_t no, v128_t when) { return wasm_v128_bitselect(yes, no, when); };
    const auto mark_set = [&](v128_t m, std::int32_t bit) {
        return wasm_i32x4_ne(wasm_v128_and(m, splat_int(bit)), no_bits);
    };
    // The four groups from i: their scores from their lanes alone. Inlined in
    // each loop, so the terms that are the same for every group are worked out
    // once per loop instead of once per four groups.
    const auto score_lanes = [&](std::size_t i) __attribute__((always_inline)) {
        {
            const v128_t index = wasm_i32x4_add(splat_int(std::int32_t(i)), wasm_i32x4_make(0, 1, 2, 3));
            const v128_t status = wasm_v128_load(&lanes.status[i]), path = wasm_v128_load(&lanes.path[i]);
            const v128_t mark = wasm_v128_load(&lanes.marks[i]);
            const v128_t is_held = wasm_i32x4_eq(status, splat_int(CampGroup::capture));
            const v128_t is_cooling = wasm_i32x4_eq(status, splat_int(CampGroup::cooling));
            const v128_t is_here = wasm_i32x4_eq(index, splat_int(here));
            const v128_t ally_here = wasm_v128_and(is_held, mark_set(mark, holder_on));
            const v128_t enemy_here = wasm_v128_and(is_held, mark_set(mark, enemy_on));
            // Not a candidate: a squad member's objective (unless we hold it),
            // out of reach, or held by an ally with no enemy contesting it.
            v128_t skip =
                wasm_v128_or(wasm_v128_or(wasm_v128_andnot(mark_set(mark, reserved), is_here),
                                          wasm_i32x4_eq(path, splat_int(bc::distance_field::unreachable))),
                             wasm_v128_andnot(wasm_v128_andnot(ally_here, is_here), enemy_here));
            if constexpr (policy::worker_pure_resource)
                skip = wasm_v128_or(skip, enemy_here); // Contesting a held reward is the squad's job.
            if constexpr (policy::single_holder)
                skip = wasm_v128_or(skip, wasm_i32x4_eq(index, splat_int(excluded)));
            const v128_t travel_steps = wasm_i32x4_max(wasm_i32x4_sub(path, splat_int(range)), no_bits);
            const v128_t waiting_steps = wasm_v128_and(
                is_cooling, wasm_i32x4_max(wasm_i32x4_sub(wasm_i32x4_sub(wasm_v128_load(&lanes.respawn[i]),
                                                                         splat_int(int(round))),
                                                          travel_steps),
                                           no_bits));
            const v128_t splash = wasm_v128_load(&lanes.splash[i]),
                         remaining = wasm_v128_load(&lanes.remaining[i]);
            const v128_t pressure = wasm_v128_load(&lanes.pressure[i]),
                         claimants = wasm_v128_load(&lanes.claims[i]);
            // dps(g).
            v128_t rate;
            if (splash_passive) {
                // spell_dps(spells, strength, focus, splash).
                rate = wasm_f32x4_mul(splat(strength), splash);
                for (const auto &spell : spells) {
                    const v128_t cooldown = splat(float(spell.cooldown));
                    if (spell.kind == bc::ability_kind::attack_reset)
                        rate = wasm_f32x4_add(
                            rate, wasm_f32x4_div(wasm_f32x4_mul(splat(strength), splash), cooldown));
                    if (spell.kind == bc::ability_kind::damage || spell.kind == bc::ability_kind::cc)
                        rate = wasm_f32x4_add(rate, splat(focus * spell.modifier / spell.cooldown));
                    if (spell.kind == bc::ability_kind::aoe_damage)
                        rate = wasm_f32x4_add(
                            rate,
                            wasm_f32x4_div(wasm_f32x4_mul(splat(focus * spell.modifier), splash), cooldown));
                }
            } else {
                rate = splat(flat_dps);
                for (const auto &spell : spells)
                    if (spell.kind == bc::ability_kind::aoe_damage)
                        rate =
                            wasm_f32x4_add(rate, wasm_f32x4_div(wasm_f32x4_mul(splat(focus * spell.modifier),
                                                                               wasm_f32x4_sub(splash, one)),
                                                                splat(float(spell.cooldown))));
            }
            if (passive == bc::passive_type::retaliation)
                rate = wasm_f32x4_add(rate, splat(retaliation));
            const v128_t helpers = wasm_f32x4_add(claimants, splat(float(free_partners)));
            const v128_t speed =
                wasm_f32x4_mul(wasm_f32x4_pmax(one, wasm_f32x4_mul(rate, splat(camp_multiplier))),
                               wasm_f32x4_add(one, wasm_f32x4_mul(helpers, splat(policy::helper_speed))));
            v128_t clear = wasm_f32x4_div(remaining, speed);
            // clear_loss(g, clear).
            v128_t loss = wasm_f32x4_mul(wasm_f32x4_mul(pressure, clear), splat(camp_mitigation));
            loss = pick(wasm_f32x4_mul(loss, splat(0.68f)), loss, wasm_v128_load(&lanes.several[i]));
            for (const auto &spell : spells) {
                const v128_t cooldown = splat(float(spell.cooldown));
                if (spell.kind == bc::ability_kind::shield)
                    loss = wasm_f32x4_sub(
                        loss, wasm_f32x4_mul(splat(focus * spell.modifier),
                                             wasm_f32x4_add(one, wasm_f32x4_div(clear, cooldown))));
                if (spell.kind == bc::ability_kind::cc)
                    loss = wasm_f32x4_sub(loss, wasm_f32x4_div(wasm_f32x4_mul(pressure, clear), cooldown));
            }
            loss = wasm_f32x4_sub(loss, wasm_f32x4_mul(splat(nearby_healing), clear));
            loss = wasm_f32x4_pmax(zero, loss);
            v128_t risk = wasm_f32x4_div(wasm_f32x4_pmax(zero, wasm_f32x4_sub(loss, splat(spare))),
                                         splat(health_scale));
            const v128_t x = wasm_v128_load(&lanes.x[i]), y = wasm_v128_load(&lanes.y[i]);
            if (issac_link)
                for (const auto &enemy : enemies) {
                    const float added = (enemy.strength + enemy.burst) * link * capture_time / health_scale;
                    const v128_t reach =
                        wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(splat_int(enemy.position.x), x)),
                                       wasm_i32x4_abs(wasm_i32x4_sub(splat_int(enemy.position.y), y)));
                    risk = pick(wasm_f32x4_add(risk, splat(added)), risk,
                                wasm_i32x4_le(reach, splat_int(enemy.spell_reach + enemy.range)));
                }
            v128_t ranged_dps = zero;
            for (const auto &spell : spells) {
                const bool area = spell.kind == bc::ability_kind::aoe_damage;
                if (spell.range > 3 &&
                    (spell.kind == bc::ability_kind::damage || spell.kind == bc::ability_kind::cc || area))
                    ranged_dps = wasm_f32x4_add(
                        ranged_dps,
                        wasm_f32x4_mul(splat(focus * spell.modifier / spell.cooldown), area ? splash : one));
            }
            // siege_open is prepared in the background; unknown counts as closed.
            // A slower but lossless range-four clear is an offensive option,
            // where the terrain leaves a tile out of every camp's reach.
            const v128_t siege =
                wasm_f32x4_div(remaining, wasm_f32x4_mul(ranged_dps, splat(camp_multiplier)));
            const v128_t sieging = wasm_v128_and(
                wasm_v128_and(
                    wasm_v128_and(wasm_f32x4_gt(risk, splat(0.15f)), wasm_f32x4_gt(ranged_dps, zero)),
                    wasm_v128_load(&lanes.siege_open[i])),
                wasm_f32x4_lt(siege,
                              wasm_f32x4_mul(clear, wasm_f32x4_add(one, wasm_f32x4_mul(splat(3.0f), risk)))));
            clear = pick(siege, clear, sieging);
            risk = pick(splat(0.1f), risk, sieging);
            clear = pick(zero, clear, is_held);
            risk = pick(zero, risk, is_held);
            // Every camp counts, and on a fortress-gem map the gem half of the
            // reward dominates: a gold cluster pays 0 gems, so no amount of gold
            // route play converts it into a win.
            const v128_t reward =
                wasm_f32x4_add(wasm_v128_load(&lanes.gem_reward[i]),
                               wasm_f32x4_mul(splat(gold_value), wasm_v128_load(&lanes.gold[i])));
            v128_t waiting = wasm_f32x4_convert_i32x4(waiting_steps);
            if constexpr (policy::respawn_wait_weight != 1.0f)
                waiting = wasm_f32x4_mul(waiting, splat(policy::respawn_wait_weight));
            v128_t duration = wasm_f32x4_add(
                wasm_f32x4_add(wasm_f32x4_add(wasm_f32x4_add(wasm_f32x4_convert_i32x4(travel_steps), clear),
                                              waiting),
                               splat(float(capture_time))),
                one);
            duration = wasm_f32x4_add(duration, wasm_v128_load(&lanes.altar[i]));
            v128_t value = wasm_f32x4_div(reward, wasm_f32x4_pmax(one, duration));
            const v128_t risk_weight = pick(splat(gem_risk), one, wasm_v128_load(&lanes.gem[i]));
            // The unshared claims term, (claims - partners) * 0.55, is zero.
            value = wasm_f32x4_div(
                value, wasm_f32x4_add(wasm_f32x4_add(one, wasm_f32x4_mul(wasm_f32x4_mul(risk, splat(4.0f)),
                                                                         risk_weight)),
                                      wasm_f32x4_mul(claimants, splat(policy::partner_penalty))));
            if constexpr (policy::worker_pure_resource) {
                // Safe side of the map, away from recent enemy sightings, and
                // camps it can clear alone with its reserve intact.
                value = pick(wasm_f32x4_mul(value, splat(policy::worker_enemy_side_penalty)), value,
                             wasm_v128_load(&lanes.enemy_side[i]));
                value = pick(wasm_f32x4_mul(value, splat(policy::worker_sighting_penalty)), value,
                             wasm_v128_load(&lanes.sighted[i]));
                value = pick(wasm_f32x4_mul(value, splat(0.05f)), value, wasm_f32x4_gt(risk, zero));
            } else {
                value = pick(wasm_f32x4_mul(value, splat(1.4f)), value, enemy_here);
                // While the squad is depleted, converge on the allies still on
                // the map instead of spreading out and feeding into a grouped
                // enemy one at a time.
                value = pick(wasm_f32x4_mul(value, splat(contest_factor)), value,
                             wasm_v128_load(&lanes.contested[i]));
                if (depleted)
                    value = pick(wasm_f32x4_mul(value, splat(policy::depleted_rally_bonus)), value,
                                 wasm_f32x4_gt(claimants, zero));
            }
            if (team > 0) {
                v128_t nearest = splat_int(std::numeric_limits<std::int32_t>::max());
                for (int k = 0; k < team; ++k)
                    nearest = wasm_i32x4_min(
                        nearest, wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(splat_int(team_x[k]), x)),
                                                wasm_i32x4_abs(wasm_i32x4_sub(splat_int(team_y[k]), y))));
                const v128_t excess = wasm_f32x4_convert_i32x4(
                    wasm_i32x4_max(no_bits, wasm_i32x4_sub(nearest, splat_int(policy::cohesion_radius))));
                value = wasm_f32x4_div(
                    value,
                    wasm_f32x4_add(one, wasm_f32x4_div(wasm_f32x4_mul(splat(policy::cohesion_weight), excess),
                                                       splat(float(policy::cohesion_radius)))));
            }
            value = pick(wasm_f32x4_mul(value, splat(policy::worker_commit_bonus)), value,
                         wasm_i32x4_eq(index, splat_int(target)));
            value = pick(wasm_f32x4_mul(value, splat(5.0f)), value, wasm_v128_and(is_held, is_here));
            // Low-health units keep attacking harmless resources instead of
            // wasting a long reset whenever that still produces score safely.
            if (weak)
                value = pick(wasm_f32x4_mul(value, splat(0.1f)), value,
                             wasm_v128_and(wasm_f32x4_gt(pressure, zero), wasm_f32x4_eq(ranged_dps, zero)));
            wasm_v128_store(&lanes.scores[i], pick(splat(skipped), value, skip));
        }
    };
    // A group whose walk is still a lower bound (past the bounded search)
    // scores at least what it will; while one could match the best, the
    // search goes further. Only walks change then, so only the groups whose
    // walk did are scored again (four at a time). Scores are rounded floats,
    // hence the margin.
    float best = -1;
    int choice = -1;
    gather();
    for (std::size_t i = 0; i < padded; i += 4)
        score_lanes(i);
    for (;;) {
        best = -1;
        choice = -1;
        float bounded_best = skipped;
        for (std::size_t i = 0; i < count; ++i)
            if (group_bounded[i])
                bounded_best = std::max(bounded_best, lanes.scores[i]);
            else if (lanes.scores[i] > best) {
                best = lanes.scores[i];
                choice = int(i);
            }
        if (bounded_best < best - std::abs(best) * 1e-4f || path_search >= 3)
            break;
        measure_paths(path_search + 1);
        for (std::size_t i = 0; i < padded; i += 4) {
            bool changed = false;
            for (std::size_t k = i; k < i + 4 && k < count; ++k)
                if (lanes.path[k] != group_path[k]) {
                    lanes.path[k] = group_path[k];
                    changed = true;
                }
            if (changed)
                score_lanes(i);
        }
    }
    last_plan = round;
    return choice;
}

void Bot::worker_farm() {
    profiling::Scope scope(profiling::Stage::farm);
    auto &me = bc::self();
    const bool finished = target >= 0 && groups[target].status == CampGroup::cooling;
    // At most one choice per turn: one made earlier this turn stands.
    if (last_plan != int(round) &&
        (target < 0 || finished || int(round) - last_plan >= policy::replan_period)) {
        const int before = target;
        target = worker_choose_group();
        // Diagnostic: abandoning an unfinished camp for another one.
        if (before >= 0 && target >= 0 && target != before && !finished)
            ++target_switches;
    }
    // One holder per capture: the reward is the team's whoever holds it, so
    // the others take the next camp instead of waiting on this one.
    if (policy::single_holder && target >= 0 && last_plan != int(round) &&
        groups[target].status == CampGroup::capture && enemies.empty() && capture_outranked(groups[target]))
        target = worker_choose_group(target);
    if (target < 0) {
        publish(0);
        return;
    }
    auto &g = groups[target];
    publish(target < TeamMemory::bank - 1 ? target + 1 : 0);
    if (distance(position, g.centre) > range + 3 && crystal_travel(target, g.tiles()))
        return;
    if (g.status == CampGroup::capture) {
        if (!g.tiles().is_set(position))
            move_to(g.tiles(), false, true);
        combat(false, target); // Heals and shields still work while holding a camp.
        ward_ahead(g.centre);
        return;
    }
    if (g.status == CampGroup::cooling) {
        auto staging = around(g.centre, range + 1);
        subtract(staging, g.tiles());
        if (!staging.is_set(position))
            move_to(staging);
        combat(false, target);
        ward_ahead(g.centre);
        return;
    }
    float clear = g.health / dps(g);
    float spell_range = 0;
    for (const auto &s : spells)
        if (s.modifier > 0 && (s.kind == bc::ability_kind::damage || s.kind == bc::ability_kind::cc ||
                               s.kind == bc::ability_kind::aoe_damage))
            spell_range = std::max(spell_range, s.range);
    bool siege = (range > 1 || policy::siege_melee) && spell_range > 3 &&
                 clear_loss(g, clear) > health * (1 - policy::casualty_margin);
    if (siege && policy::siege_alone_only && ally_draws_attacks(g))
        siege = false;
    bc::bitboard goals;
    const CampMemory *nearest = nullptr;
    int best_distance = bc::distance_field::unreachable;
    for (const auto &c : g.camps) {
        if (c.health <= 0)
            continue;
        int d = distance(position, c.position);
        if (d < best_distance) {
            best_distance = d;
            nearest = &c;
        }
        goals.draw_chebyshev(c.position, float(range)); // Adds tiles: no temporary mask per camp.
    }
    subtract(goals, blocked);
    if (siege) {
        // Stand within spell reach but out of every camp's reach. Without such
        // a tile, attack from normal range rather than wait for one.
        auto stand = siege_tiles(g, spell_range, false);
        subtract(stand, blocked);
        if (stand.is_empty())
            siege = false;
        else
            goals = std::move(stand);
    }
    if (passive == bc::passive_type::splash_attack && !siege) {
        // Stand beside the best splash centre. Stopping at the nearest edge
        // of a row can halve Varg's damage for the entire camp clear.
        float best = -1;
        const CampMemory *best_primary = nullptr;
        for (const auto &primary : g.camps)
            if (primary.health > 0) {
                // Tested in a reused mask; only the winner's is kept.
                search_goals.clear();
                search_goals.draw_chebyshev(primary.position, float(range));
                subtract(search_goals, blocked);
                if (search_goals.is_empty())
                    continue;
                float damage = 0;
                for (const auto &other : g.camps)
                    if (distance(primary.position, other.position) <=
                        bc::constants::passive_abilities::varg_splash_radius)
                        damage += std::min(strength, std::max(0.0f, other.health));
                float value =
                    damage - strength * 0.1f * std::max(0, distance(position, primary.position) - range);
                if (value > best) {
                    best = value;
                    best_primary = &primary;
                }
            }
        if (best_primary) {
            goals = around(best_primary->position, range);
            subtract(goals, blocked);
        }
    }
    if (!nearest) {
        target = -1;
        return;
    }
    if (siege && !goals.is_set(position)) {
        move_to(goals, true, false, target);
        // Walking precludes ranged damage spells this turn.
        if (walked) {
            ward_ahead(nearest->position);
            return;
        }
    }
    if (range <= 1 && !goals.is_set(position) && !siege) {
        if (move_to(goals, false, true, target))
            observe_after_move(target);
    }
    combat(false, target);
    if (!me.is_alive())
        return;
    if (me.can_move() && !goals.is_set(position)) {
        if (move_to(goals, false, true, target)) {
            observe_after_move(target);
            combat(false, target);
        }
    }
    // Move onto the cleared region immediately with the melee move allowance.
    if (g.seen == int(round)) {
        bc::vision::visible_tiles_into(occupancy);
        occupancy &= g.tiles();
        int timer = 0;
        if (!occupancy.is_empty() && group_state(g, timer) == 2) {
            g.status = CampGroup::capture;
            refresh_blocked();
            if (!g.tiles().is_set(position))
                move_to(g.tiles(), false, false);
        }
    }
    ward_ahead(nearest->position);
}

void Bot::prepare_sieges() {
    // Whether each group leaves a tile for a spell-range siege: terrain and
    // camp ranges are fixed, so check two groups per turn in the background.
    if (siege_reach <= 0 || siege_cursor >= groups.size())
        return;
    for (int checked = 0; checked < 2 && siege_cursor < groups.size(); ++checked, ++siege_cursor)
        groups[siege_cursor].siege_open = !siege_tiles(groups[siege_cursor], siege_reach, true).is_empty();
}

bc::bitboard Bot::siege_tiles(const CampGroup &g, float reach, bool assume_alive) const {
    // Within spell reach of a camp, outside the attack range of every camp
    // that strikes back: a clear the camps cannot answer.
    bc::bitboard tiles, threatened;
    for (const auto &c : g.camps) {
        if (!assume_alive && c.health <= 0)
            continue;
        tiles.draw_chebyshev(c.position, reach);
        if (c.stats.base_strength > 0)
            threatened.draw_chebyshev(c.position, c.stats.base_attack_range);
    }
    // In place: tiles & ~threatened & ~static_blocked.
    tiles &= open_terrain;
    threatened &= tiles;
    tiles ^= threatened;
    return tiles;
}

bool Bot::capture_outranked(const CampGroup &g) const {
    // A visible ally on the capture holds it instead of us: a carry before
    // anyone else; between equals, any holder when we are off the tiles, then
    // the bigger bag, then the lower id. Every holder ranks the same way, so
    // exactly one stays, and a carry arriving takes over from a non-carry.
    const bool on = g.tiles().is_set(position);
    for (const auto &ally : allies) {
        if (ally.health <= 0 || !g.tiles().is_set(ally.position))
            continue;
        if (carry(ally.type) != carry(type) ? carry(ally.type)
            : !on                           ? true
            : ally.gems != int(gems)        ? ally.gems > int(gems)
                                            : id(ally.champion()) < id(bc::self()))
            return true;
    }
    return false;
}

bool Bot::ally_draws_attacks(const CampGroup &g) const {
    // Camps attack the nearest champion: an ally closer to a striking camp
    // than our attack range takes the hits while we attack.
    for (const auto &ally : allies)
        for (const auto &c : g.camps)
            if (c.health > 0 && c.stats.base_strength > 0 && distance(ally.position, c.position) < range)
                return true;
    return false;
}
