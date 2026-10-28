// Worker camp choice and farming.
#include "../bot.h"
#include <wasm_simd128.h>

int Bot::worker_choose_group(int excluded) {
    // A group we were walled off from lately is no candidate.
    const int walled = int(round) < stuck_group_until ? stuck_group : -1;
    profiling::Scope scope(profiling::Stage::choose);
    if (hops.size() != groups.size())
        build_hops();
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
    // Per group, from one pass over the claims and the units in view:
    // teammates' claims (each claimant can supply the next clear's damage, so
    // each claim is also a partner), and an ally or an enemy standing on it.
    enum : std::uint8_t { ally_on = 2, enemy_on = 4 };
    static std::vector<std::uint8_t> claims, marks;
    claims.assign(groups.size(), 0);
    marks.assign(groups.size(), 0);
    for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
        if (s != slot && living(s) && shared.get(s) >= 1 && shared.get(s) <= groups.size())
            ++claims[shared.get(s) - 1];
    // Each teammate's damage per round on camps (from its live strength and
    // focus in view, else its type's base) and, per camp, its claimants'
    // summed.
    std::array<float, TeamMemory::slots> mate_rate{};
    const unsigned kits = unsigned(std::min<std::size_t>(roster_kits.size(), TeamMemory::slots));
    for (unsigned s = 0; s < kits; ++s)
        mate_rate[s] = roster_kits[s].camp_rate;
    for (const auto &ally : allies)
        for (unsigned s = 0; s < kits; ++s)
            if (ally.champion() == roster[s]) {
                const auto &kit = roster_kits[s];
                mate_rate[s] = camp_dps(kit.spells, ally.strength, ally.focus, 1) * kit.camp_multiplier;
                break;
            }
    claim_rates.assign(groups.size(), 0.0f);
    for (unsigned s = 0; s < kits; ++s)
        if (s != slot && living(s) && shared.get(s) >= 1 && shared.get(s) <= groups.size())
            claim_rates[shared.get(s) - 1] += mate_rate[s];
    const auto standing_on = [&](vec2 p) {
        const int g = group_at(p);
        return g >= 0 && distance(p, groups[g].centre) <= int(groups[g].camps.size()) ? g : -1;
    };
    // An ally on a camp holds it only if it claims it; one crossing the tiles
    // or retreating over them leaves the next turn. A held camp is left to
    // its holder.
    for (const auto &a : allies)
        if (const int g = standing_on(a.position); g >= 0 && claimed_by(a) == g)
            marks[g] |= ally_on;
    for (const auto &e : enemies)
        if (const int g = standing_on(e.position); g >= 0)
            marks[g] |= enemy_on;
    const int here = group_at(position);
    const float gem_weight = gem_reward_weight(opening), gem_risk = gem_risk_weight(opening);
    const float behind = behind_pressure();
    const float enemy_side_mul = policy::enemy_side_factor + (1.0f - policy::enemy_side_factor) * behind;
    const float camp_risk = policy::camp_risk_weight * (1.0f - policy::behind_risk_relief * behind);
    const int capture_time = rules::settings().capture_time;
    const float health_scale = std::max(100.0f, health), spare = std::max(0.0f, health - reserve);
    // The Willow keeps to its Prism's group (policy::willow_follow).
    int prism_group = -1;
    if (type == bc::champion_type::willow)
        for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
            if (s != slot && living(s) && opening.roster_types[s] == bc::champion_type::prism &&
                shared.get(s) >= 1 && shared.get(s) <= groups.size())
                prism_group = int(shared.get(s)) - 1;
    // A group's helpers are the teammates claiming it: willow best girl's
    // champions farm apart (61-74% of the time within 6 of a teammate, two or
    // more on 32% of its clears from round 150 to 400) and join teammates'
    // groups less than ours did when nearby teammates counted as helpers on
    // every group.
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
        // Fixed: masks for a gem reward, a group on the enemy's side and a
        // safe gem group (harmless camps only); the gem reward, the gold, the
        // altar term, the camp loss factor, the camp count, the walk from home
        // and the camps' longest reach.
        std::int32_t *gem, *enemy_side, *safe_gems;
        float *gem_reward, *gold, *altar, *loss_factor, *camp_count, *home, *reach;
        // This choice's state, with the claimants' summed camp rates.
        std::int32_t *status, *respawn, *path, *siege_open, *marks, *sighted;
        float *remaining, *splash, *pressure, *claims, *claim_rate, *scores, *pair_reward, *pair_extra;
    };
    static Lanes lanes;
    if (lanes.padded != padded) {
        lanes.padded = padded;
        lanes.ints.reset(new std::int32_t[9 * padded]);
        lanes.floats.reset(new float[15 * padded]);
        std::int32_t *next_int = lanes.ints.get();
        for (auto **lane :
             {&lanes.gem, &lanes.enemy_side, &lanes.safe_gems, &lanes.status, &lanes.respawn, &lanes.path,
              &lanes.siege_open, &lanes.marks, &lanes.sighted}) {
            *lane = next_int;
            next_int += padded;
        }
        float *next_float = lanes.floats.get();
        for (auto **lane : {&lanes.gem_reward, &lanes.gold, &lanes.altar, &lanes.remaining, &lanes.splash,
                            &lanes.pressure, &lanes.claims, &lanes.scores, &lanes.loss_factor,
                            &lanes.claim_rate, &lanes.camp_count, &lanes.home, &lanes.reach,
                            &lanes.pair_reward, &lanes.pair_extra}) {
            *lane = next_float;
            next_float += padded;
        }
        for (std::size_t i = count; i < padded; ++i) {
            for (auto *lane : {lanes.gem, lanes.enemy_side, lanes.safe_gems, lanes.status, lanes.respawn,
                               lanes.siege_open, lanes.marks})
                lane[i] = 0;
            for (auto *lane : {lanes.gem_reward, lanes.gold, lanes.altar, lanes.remaining, lanes.splash,
                               lanes.pressure, lanes.claims, lanes.loss_factor, lanes.claim_rate,
                               lanes.camp_count, lanes.home, lanes.reach, lanes.pair_reward,
                               lanes.pair_extra})
                lane[i] = 0;
            lanes.path[i] = bc::distance_field::unreachable; // Padding is never chosen.
        }
        for (std::size_t i = 0; i < padded; ++i)
            lanes.sighted[i] = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto &g = groups[i];
            const auto gem_value = g.unit.gem_value();
            lanes.gem[i] = gem_value > 0 ? -1 : 0;
            lanes.enemy_side[i] = g.enemy_distance < g.home_distance ? -1 : 0;
            bool harmless = gem_value > 0;
            for (const auto &c : g.camps)
                harmless = harmless && c.stats.base_strength <= 0;
            lanes.safe_gems[i] = harmless ? -1 : 0;
            lanes.gem_reward[i] = gem_value * gem_weight;
            lanes.gold[i] = float(g.unit.gold_value());
            lanes.loss_factor[i] = camp_loss_factor(g);
            lanes.camp_count[i] = float(g.camps.size());
            lanes.home[i] = float(g.home_distance);
            float reach = 0;
            // The farthest two camps apart (Chebyshev): the larger of the
            // spans along the two axes, from one pass rather than every pair.
            int low_x = std::numeric_limits<int>::max(), high_x = std::numeric_limits<int>::min();
            int low_y = low_x, high_y = high_x;
            for (const auto &c : g.camps) {
                reach = std::max(reach, float(c.stats.base_attack_range));
                low_x = std::min(low_x, c.position.x);
                high_x = std::max(high_x, c.position.x);
                low_y = std::min(low_y, c.position.y);
                high_y = std::max(high_y, c.position.y);
            }
            const int extent = g.camps.empty() ? 0 : std::max(high_x - low_x, high_y - low_y);
            lanes.reach[i] = reach;
            // The altar term, and the walk across a group longer than two
            // attack ranges: a row of 30 gold clusters scored as one quick
            // clear and walked ours along it into the enemy's half.
            lanes.altar[i] = std::min(20, g.altar_distance) * 0.25f + float(std::max(0, extent - 2 * range));
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
            siege[i] = range > 1 && g.siege_open > 0 ? -1 : 0;
            mark[i] = marks[i];
            remaining[i] = g.status == CampGroup::cooling ? g.max_health : g.health;
            splash[i] = g.splash;
            pressure[i] = g.pressure;
            claim[i] = float(claims[i]);
        }
        for (std::size_t i = 0; i < count; ++i)
            lanes.sighted[i] = i < enemy_seen_near.size() &&
                                       int(round) - enemy_seen_near[i] <= policy::worker_sighting_memory
                                   ? -1
                                   : 0;
        for (std::size_t i = 0; i < count; ++i)
            lanes.claim_rate[i] = claim_rates[i];
    };
    const auto splat = [](float v) { return wasm_f32x4_splat(v); };
    const auto splat_int = [](std::int32_t v) { return wasm_i32x4_splat(v); };
    const v128_t one = splat(1.0f), zero = splat(0.0f), no_bits = splat_int(0);
    // Our rate on camps, with Heart of Ruin's passive in its hits and its dash
    // added per group below.
    const float flat_dps = camp_dps(spells, basic_damage(), focus, 0);
    // A damaging dash's rate per camp it lands on; the steps a champion with a
    // dash takes per tile; capture gold with our bonus.
    float dash_rate = 0, travel_scale = 1;
    for (const auto &s : spells)
        if (s.kind == bc::ability_kind::dash) {
            travel_scale = policy::dash_travel_scale;
            if (s.modifier > 0)
                dash_rate += focus * s.modifier / s.cooldown;
        }
    const float gold_weight =
        gold_value * gold_bonus * (type == bc::champion_type::willow ? policy::willow_gold_factor : 1.0f);
    // The Prism farms the safe gem groups (policy::prism_safe_gem_factor).
    const float safe_gem_factor = type == bc::champion_type::prism ? policy::prism_safe_gem_factor : 1.0f;
    // Marcella walks to the bigger groups (policy::marcella_travel_hazard).
    const float travel_hazard =
        type == bc::champion_type::marcella ? policy::marcella_travel_hazard : policy::travel_hazard;
    // Multi-visit clears: the health a visit can spend before going home,
    // the rounds left, and a trip home's recall.
    const float usable = std::max(100.0f, max_health * (1 - policy::retreat_health));
    const float rounds_left = float(int(rules::settings().max_rounds) - int(round));
    const float recall_trip = float(recall_turns + 1);
    // The members the scoring reads, copied: its stores to the lanes could
    // alias the members themselves, so each would be read again (and the
    // terms made from it worked out again) for every four groups.
    const int our_range = range, now = int(round), current = target;
    const float our_focus = focus, our_multiplier = camp_multiplier, camp_scale = camp_mitigation * mitigation,
                healing = nearby_healing;
    const std::array<Spell, 2> kit = spells;
    // Each group's best neighbour as a second leg of one plan: the neighbour's
    // reward, and the time it adds (walk, respawn wait, clear, capture).
    const float team_dps = std::max(1.0f, flat_dps * our_multiplier);
    const auto gather_pairs = [&] {
        for (std::size_t i = 0; i < count; ++i) {
            float best_rate = 0, best_reward = 0, best_extra = 0;
            for (const auto &[j, hop] : hops[i]) {
                const auto &n = groups[j];
                if (int(j) == excluded || int(j) == walled || lanes.sighted[j])
                    continue;
                if (n.status == CampGroup::capture && (claims[j] || (marks[j] & ally_on)))
                    continue;
                if (lanes.reach[j] >= 5.0f && !claims[j]) // Monoliths stay team-only.
                    continue;
                const float wait =
                    n.status == CampGroup::cooling ? float(std::max(0, n.respawn - now - hop)) : 0.0f;
                if (wait > float(policy::hop_max_wait))
                    continue;
                const float reward = lanes.gem_reward[j] + gold_weight * lanes.gold[j];
                const float extra = float(hop) * travel_scale + wait * policy::respawn_wait_weight +
                                    lanes.remaining[j] / team_dps + float(capture_time) + 1;
                if (reward / extra > best_rate) {
                    best_rate = reward / extra;
                    best_reward = reward;
                    best_extra = extra;
                }
            }
            lanes.pair_reward[i] = best_reward;
            lanes.pair_extra[i] = best_extra;
        }
    };
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
            const v128_t ally_here = wasm_v128_and(is_held, mark_set(mark, ally_on));
            const v128_t enemy_here = wasm_v128_and(is_held, mark_set(mark, enemy_on));
            // Not a candidate: out of reach, held by an ally with no enemy
            // contesting it, or the camp a holder outranks us on.
            v128_t skip = wasm_v128_or(wasm_i32x4_eq(path, splat_int(bc::distance_field::unreachable)),
                                       wasm_v128_andnot(wasm_v128_andnot(ally_here, is_here), enemy_here));
            skip = wasm_v128_or(skip, wasm_i32x4_eq(index, splat_int(excluded)));
            skip = wasm_v128_or(skip, wasm_i32x4_eq(index, splat_int(walled)));
            v128_t travel_steps = wasm_i32x4_max(wasm_i32x4_sub(path, splat_int(our_range)), no_bits);
            travel_steps = pick(path, travel_steps, is_held); // Holding means standing on a tile.
            const v128_t splash = wasm_v128_load(&lanes.splash[i]),
                         remaining = wasm_v128_load(&lanes.remaining[i]);
            const v128_t pressure = wasm_v128_load(&lanes.pressure[i]),
                         claimants = wasm_v128_load(&lanes.claims[i]);
            // Each further teammate on a group costs more: willow best girl's
            // fourth and fifth champions take the small groups near by instead.
            v128_t crowding = wasm_f32x4_mul(
                wasm_f32x4_mul(claimants, wasm_f32x4_add(claimants, one)), splat(0.5f * policy::partner_penalty));
            // A capture a teammate claims is its holder's, unless we hold it:
            // walking there only finds the tile taken.
            skip = wasm_v128_or(skip, wasm_v128_andnot(wasm_v128_and(is_held, wasm_f32x4_gt(claimants, zero)),
                                                       is_here));
            // Monoliths (reach 5) are cleared as a team: willow best girl strikes
            // them with 2.41 champions a round (3+ in 44%), ours did alone in 87%
            // and spent three times its strike rounds there.
            const v128_t monolith = wasm_f32x4_ge(wasm_v128_load(&lanes.reach[i]), splat(5.0f));
            crowding = pick(zero, crowding, monolith);
            skip = wasm_v128_or(skip, wasm_v128_andnot(wasm_v128_and(monolith, wasm_f32x4_eq(claimants, zero)),
                                                       wasm_v128_or(is_held, is_here)));
            // dps(g).
            v128_t rate = splat(flat_dps);
            for (const auto &spell : kit)
                if (spell.kind == bc::ability_kind::aoe_damage)
                    rate = wasm_f32x4_add(rate, wasm_f32x4_div(wasm_f32x4_mul(splat(our_focus * spell.modifier),
                                                                              wasm_f32x4_sub(splash, one)),
                                                               splat(float(spell.cooldown))));
            // A damaging dash lands on up to two of the camps.
            const v128_t camps_in_group = wasm_v128_load(&lanes.camp_count[i]);
            rate = wasm_f32x4_add(
                rate, wasm_f32x4_mul(splat(dash_rate), wasm_f32x4_pmin(splat(2.0f), camps_in_group)));
            const v128_t helpers = claimants;
            // The team's summed camp rates: ours and the claimants'.
            const v128_t team_rate =
                wasm_f32x4_add(wasm_f32x4_mul(rate, splat(our_multiplier)), wasm_v128_load(&lanes.claim_rate[i]));
            const v128_t speed =
                wasm_f32x4_pmax(one, wasm_f32x4_mul(team_rate, splat(policy::team_clear_share)));
            v128_t clear = wasm_f32x4_div(remaining, speed);
            // Each camp takes at least one hit.
            clear = wasm_f32x4_pmax(clear, wasm_f32x4_div(camps_in_group, wasm_f32x4_add(one, helpers)));
            // clear_loss(g, clear).
            v128_t loss = wasm_f32x4_mul(wasm_f32x4_mul(pressure, clear),
                                         splat(camp_scale));
            loss = wasm_f32x4_mul(loss, wasm_v128_load(&lanes.loss_factor[i]));
            // The clear's camp damage before splitting it between tank and
            // helpers (for multi-visit clears).
            const v128_t team_loss = loss;
            // With a helper the camps strike the tank: a ranged champion takes
            // few of the hits, a melee one more.
            loss = pick(wasm_f32x4_mul(loss, splat(our_range > 1 ? policy::ranged_helped_loss
                                                             : policy::melee_helped_loss)),
                        loss, wasm_f32x4_ge(helpers, one));
            for (const auto &spell : kit) {
                const v128_t cooldown = splat(float(spell.cooldown));
                if (spell.kind == bc::ability_kind::shield)
                    loss = wasm_f32x4_sub(
                        loss, wasm_f32x4_mul(splat(our_focus * spell.modifier),
                                             wasm_f32x4_add(one, wasm_f32x4_div(clear, cooldown))));
                if (spell.kind == bc::ability_kind::cc)
                    loss = wasm_f32x4_sub(loss, wasm_f32x4_div(wasm_f32x4_mul(pressure, clear), cooldown));
            }
            loss = wasm_f32x4_sub(loss, wasm_f32x4_mul(splat(healing), clear));
            loss = wasm_f32x4_pmax(zero, loss);
            v128_t risk = wasm_f32x4_div(wasm_f32x4_pmax(zero, wasm_f32x4_sub(loss, splat(spare))),
                                         splat(health_scale));
            v128_t ranged_dps = zero;
            for (const auto &spell : kit) {
                const bool area = spell.kind == bc::ability_kind::aoe_damage;
                // Area spells, and the targeted spells that can target a camp.
                if (spell.range > 3 &&
                    (spell.kind == bc::ability_kind::damage || spell.kind == bc::ability_kind::cc || area) &&
                    (area || spell.targets.contains(bc::ability_target_mask::camps)))
                    ranged_dps = wasm_f32x4_add(
                        ranged_dps,
                        wasm_f32x4_mul(splat(our_focus * spell.modifier / spell.cooldown), area ? splash : one));
            }
            // siege_open is prepared in the background; unknown counts as closed.
            // A slower but lossless range-four clear is an offensive option,
            // where the terrain leaves a tile out of every camp's reach.
            const v128_t siege =
                wasm_f32x4_div(remaining, wasm_f32x4_mul(ranged_dps, splat(our_multiplier)));
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
                               wasm_f32x4_mul(splat(gold_weight), wasm_v128_load(&lanes.gold[i])));
            // A champion with a dash travels the faster, and so waits the
            // longer for a respawn.
            const v128_t travel = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(travel_steps), splat(travel_scale));
            const v128_t respawn_in = wasm_f32x4_convert_i32x4(
                wasm_i32x4_sub(wasm_v128_load(&lanes.respawn[i]), splat_int(now)));
            const v128_t waiting = wasm_f32x4_mul(
                wasm_v128_and(is_cooling, wasm_f32x4_pmax(zero, wasm_f32x4_sub(respawn_in, travel))),
                splat(policy::respawn_wait_weight));
            v128_t duration =
                wasm_f32x4_add(wasm_f32x4_add(wasm_f32x4_add(wasm_f32x4_add(travel, clear), waiting),
                                              splat(float(capture_time))),
                               one);
            duration = wasm_f32x4_add(duration, wasm_v128_load(&lanes.altar[i]));
            v128_t value = wasm_f32x4_div(reward, wasm_f32x4_pmax(one, duration));
            value = wasm_f32x4_div(value, wasm_f32x4_add(one, wasm_f32x4_mul(duration, splat(policy::plan_hazard))));
            const v128_t risk_weight = pick(splat(gem_risk), one, wasm_v128_load(&lanes.gem[i]));
            // The unshared claims term, (claims - partners) * 0.55, is zero.
            value = wasm_f32x4_div(
                value, wasm_f32x4_add(wasm_f32x4_add(one, wasm_f32x4_mul(wasm_f32x4_mul(risk, splat(camp_risk)),
                                                                         risk_weight)),
                                      crowding));
            // Camp damage persists, so a clear longer than our health lasts
            // can be paid in trips home (a recall and the walk back) instead
            // of in the risk of dying, and a clear by several champions shares
            // its damage: camps hit the nearest, and the tank leaves when hurt
            // for the next to take it. Keep the better of the two plans; none
            // finishes after the last round.
            // Each visit ends with a walk out of the camps' reach under their
            // fire (a hit cancels the recall), from our attack range: the
            // health a visit can spend leaves that much for it, and a visit
            // that cannot afford it is no plan (alone beside a monolith pair,
            // whose reach of 5 outranges us all, ours died within 3 rounds).
            const v128_t exit_steps =
                wasm_f32x4_pmax(zero, wasm_f32x4_sub(wasm_v128_load(&lanes.reach[i]), splat(float(our_range) - 1)));
            const v128_t visit_health = wasm_f32x4_sub(
                splat(usable), wasm_f32x4_mul(wasm_f32x4_mul(pressure, exit_steps), splat(camp_scale)));
            const v128_t share = wasm_f32x4_div(team_loss, wasm_f32x4_add(one, helpers));
            const v128_t excess = wasm_f32x4_pmax(zero, wasm_f32x4_sub(share, splat(spare)));
            const v128_t trips =
                pick(zero, wasm_f32x4_ceil(wasm_f32x4_div(excess, wasm_f32x4_pmax(splat(100.0f), visit_health))),
                     wasm_v128_or(sieging, is_held));
            const v128_t trip_time = wasm_f32x4_add(
                splat(recall_trip), wasm_f32x4_mul(wasm_v128_load(&lanes.home[i]), splat(travel_scale)));
            const v128_t visits_duration = wasm_f32x4_add(duration, wasm_f32x4_mul(trips, trip_time));
            const v128_t visit_risk = wasm_f32x4_pmin(risk, splat(policy::visit_risk));
            v128_t visits = wasm_f32x4_div(reward, wasm_f32x4_pmax(one, visits_duration));
            visits = wasm_f32x4_div(visits, wasm_f32x4_add(one, wasm_f32x4_mul(visits_duration, splat(policy::plan_hazard))));
            visits = wasm_f32x4_div(
                visits, wasm_f32x4_add(wasm_f32x4_add(one, wasm_f32x4_mul(wasm_f32x4_mul(visit_risk, splat(camp_risk)),
                                                                         risk_weight)),
                                       crowding));
            visits = pick(zero, visits,
                          wasm_v128_or(wasm_f32x4_gt(visits_duration, splat(rounds_left)),
                                       wasm_v128_andnot(wasm_f32x4_lt(visit_health, splat(100.0f)), is_held)));
            value = pick(zero, value, wasm_f32x4_gt(duration, splat(rounds_left)));
            value = wasm_f32x4_pmax(value, visits);
            {
                const v128_t pr = wasm_v128_load(&lanes.pair_reward[i]);
                const v128_t px = wasm_v128_load(&lanes.pair_extra[i]);
                const v128_t pair_duration = wasm_f32x4_add(duration, px);
                v128_t pair = wasm_f32x4_div(wasm_f32x4_add(reward, pr), wasm_f32x4_pmax(one, pair_duration));
                pair = wasm_f32x4_div(
                    pair, wasm_f32x4_add(one, wasm_f32x4_mul(pair_duration, splat(policy::plan_hazard))));
                pair = wasm_f32x4_div(
                    pair, wasm_f32x4_add(wasm_f32x4_add(one, wasm_f32x4_mul(wasm_f32x4_mul(risk, splat(camp_risk)),
                                                                            risk_weight)),
                                         crowding));
                pair = pick(zero, pair, wasm_f32x4_gt(pair_duration, splat(rounds_left)));
                pair = pick(zero, pair, wasm_f32x4_le(pr, zero)); // No neighbour: no pair.
                value = wasm_f32x4_pmax(value, wasm_f32x4_mul(pair, splat(policy::chain_discount)));
            }
            value = wasm_f32x4_div(
                value, wasm_f32x4_add(one, wasm_f32x4_mul(wasm_f32x4_add(travel, waiting), splat(travel_hazard))));
            value = pick(wasm_f32x4_mul(value, splat(enemy_side_mul)), value,
                         wasm_v128_load(&lanes.enemy_side[i]));
            value = pick(wasm_f32x4_mul(value, splat(safe_gem_factor)), value, wasm_v128_load(&lanes.safe_gems[i]));
            // No visit affords the walk out: no plan at all. The single clear
            // kept a tiny score and won once the groups near were down: our
            // champions struck a monolith pair alone from full health and died
            // (0.88 deaths a game on monolith maps, willow best girl's none).
            skip = wasm_v128_or(skip, wasm_v128_andnot(wasm_f32x4_lt(visit_health, splat(100.0f)), is_held));
            value = pick(wasm_f32x4_mul(value, splat(1.4f)), value, enemy_here);
            // Camps by a stronger group we broke contact with wait out the
            // sighting (worker_sighting_memory).
            value = pick(wasm_f32x4_mul(value, splat(policy::worker_sighting_penalty)), value,
                         wasm_v128_load(&lanes.sighted[i]));
            // While the squad is depleted, converge on the allies still on
            // the map instead of spreading out and feeding into a grouped
            // enemy one at a time.
            if (depleted)
                value = pick(wasm_f32x4_mul(value, splat(policy::depleted_rally_bonus)), value,
                             wasm_f32x4_gt(claimants, zero));
            value = pick(wasm_f32x4_mul(value, splat(policy::worker_commit_bonus)), value,
                         wasm_i32x4_eq(index, splat_int(current)));
            value = pick(wasm_f32x4_mul(value, splat(policy::willow_follow)), value,
                         wasm_i32x4_eq(index, splat_int(prism_group)));
            value = pick(wasm_f32x4_mul(value, splat(5.0f)), value, wasm_v128_and(is_held, is_here));
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
    gather_pairs();
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
    // Walled off from the target (camp bodies sealing the only corridor and
    // no visit affordable through them): ours stood still beside a monolith
    // pair for 15-40 rounds. Leave it out for a while and choose another.
    if (target >= 0 && farm_stalls >= policy::stuck_turns && last_plan != int(round)) {
        stuck_group = target;
        stuck_group_until = int(round) + policy::stuck_rounds;
        farm_stalls = 0;
        target = worker_choose_group(target);
    }
    // This turn's walk: away from the target with no step, strike or cast.
    struct FarmStalls {
        Bot &bot;
        vec2 from;
        bool counted = false; // A nested worker_farm() counts this turn.
        ~FarmStalls() {
            if (counted)
                return;
            const auto &self = bc::self();
            const int t = bot.target;
            // Reads only, so their order changes nothing: the tests without
            // engine calls first.
            const auto ready = bc::champion_state::can_attack | bc::champion_state::can_cast;
            const bool idle = bot.position == from && t >= 0 &&
                              distance(bot.position, bot.groups[t].centre) > bot.range + 2 && self.is_alive() &&
                              self.state().contains(ready) && !bot.groups[t].tiles().is_set(bot.position);
            bot.farm_stalls = idle ? bot.farm_stalls + 1 : 0;
        }
    } farm_stalls_count{*this, position};
    const bool finished = target >= 0 && groups[target].status == CampGroup::cooling;
    // A capture a few steps away that we outrank its holders on (the bigger
    // bag or first there, capture_outranked()) is ours to step onto: camp choice, which
    // leaves captures to the teammates on them, does not take it from us.
    const bool taking = target >= 0 && groups[target].status == CampGroup::capture &&
                        distance(position, groups[target].centre) <= range + 2 && !capture_outranked(groups[target]);
    // At most one choice per turn: one made earlier this turn stands.
    if (last_plan != int(round) &&
        (target < 0 || finished || (!taking && int(round) - last_plan >= policy::replan_period))) {
        const int before = target;
        target = worker_choose_group();
        // Diagnostic: abandoning an unfinished camp for another one.
        if (before >= 0 && target >= 0 && target != before && !finished)
            ++target_switches;
    }
    // One holder per capture: the reward is the team's whoever holds it, so
    // the others take the next camp instead of waiting on this one, enemies
    // in view or not (a fight is skirmish's, before farming). The top three
    // hold 95% of their captures with one champion.
    if (target >= 0 && last_plan != int(round) && groups[target].status == CampGroup::capture &&
        capture_outranked(groups[target]))
        target = worker_choose_group(target);
    // The group just chosen may be one kept out of, and the one left be
    // one exempted (mark_camp_zones()).
    if (camp_zone_round == int(round) || zone_target != target) {
        mark_camp_zones();
        refresh_blocked();
    }
    if (target < 0 || groups[target].status != CampGroup::capture)
        capture_since = -1;
    if (target < 0) {
        publish(0);
        return;
    }
    auto &g = groups[target];
    publish(target < TeamMemory::bank - 1 ? target + 1 : 0);
    if (g.status == CampGroup::capture) {
        if (!g.tiles().is_set(position) && capture_deferred(g) && last_plan != int(round)) {
            const int left = target;
            target = worker_choose_group(target);
            if (target >= 0 && target != left) {
                farm_stalls_count.counted = true;
                worker_farm();
            }
            return;
        }
        if (!g.tiles().is_set(position))
            move_to(g.tiles(), g.centre, false, true);
        // The round we stood on it from (unknown ones count from now): our
        // capture's timer runs from the next round's capture phase at the
        // latest.
        if (!g.tiles().is_set(position))
            capture_since = -1;
        else {
            if (capture_since < 0 || capture_group != target) {
                capture_since = int(round);
                capture_group = target;
            }
            homeward = home_after_capture(g);
        }
        combat(false, target); // Heals and shields still work while holding a camp.
        ward_ahead(g.centre);
        return;
    }
    if (g.status == CampGroup::cooling) {
        auto staging = around(g.centre, range + 1);
        subtract(staging, g.tiles());
        if (!staging.is_set(position))
            move_to(staging, g.centre);
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
    // Spell-range sieges are for ranged casters, and only when no ally is
    // closer to the camp: camps attack the nearest champion, so with a closer
    // ally we attack from normal range.
    bool siege =
        range > 1 && spell_range > 3 && clear_loss(g, clear) > health * (1 - policy::casualty_margin);
    if (siege && ally_draws_attacks(g))
        siege = false;
    // A mask kept between turns, cleared: a new one costs an allocation.
    auto &goals = farm_goals;
    goals.clear();
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
    if (!nearest) {
        // Every camp remembered dead though the group lives: it respawned
        // unseen. Go and look instead of standing idle.
        if (move_to(around(g.centre, range), g.centre, false, true))
            return;
        target = -1;
        return;
    }
    // In range is not in sight: a camp behind a wall cannot be hit, and what
    // we remember of it can be stale. Ranged champions stood in range of
    // camps they could not see for 20-140 rounds at a time (a sixth of
    // Vesper's turns on corridor maps). With none of the group's living camps
    // in view from a tile in range, close in beside the nearest one until one
    // is in sight from a tile in range, even where that route leaves the
    // range. Switching goals as range or sight came and went sent champions
    // back and forth between two tiles for up to 145 rounds.
    bool sighted = false;
    for (const auto &c : g.camps)
        sighted = sighted || (c.health > 0 && visible.is_set(c.position));
    // Closing in ends once a camp of the group is in sight from a tile in
    // range, or once we struck one last turn: a ranged champion striking
    // from outside the remembered range was otherwise sent beside the camps.
    const bool in_range = goals.is_set(position);
    if (unseen_group != target || (sighted && (in_range || g.attacked >= int(round) - 1)))
        unseen_group = -1;
    if (!sighted && in_range)
        unseen_group = target;
    if (unseen_group == target) {
        auto beside = around(nearest->position, 1);
        subtract(beside, blocked);
        if (!beside.is_empty()) {
            goals = std::move(beside);
            siege = false;
        }
    }
    if (siege && !goals.is_set(position)) {
        move_to(goals, nearest->position, true, false, target);
        // Walking precludes ranged damage spells this turn.
        if (walked) {
            ward_ahead(nearest->position);
            return;
        }
    }
    if (range <= 1 && !goals.is_set(position) && !siege) {
        if (move_to(goals, nearest->position, false, true, target))
            observe_after_move(target);
    }
    combat(false, target);
    if (!me.is_alive())
        return;
    if (me.can_move() && !goals.is_set(position)) {
        if (move_to(goals, nearest->position, false, true, target)) {
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
            if (!g.tiles().is_set(position) && !capture_deferred(g))
                move_to(g.tiles(), g.centre, false, false);
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

bool Bot::reports_fight(const ObservedUnit &ally) const {
    // An ally whose published code asks for help in a fight.
    for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
        if (s != slot && ally.champion() == roster[s])
            return TeamMemory::requested_force(shared.get(s), groups.size()) > 0;
    return false;
}

int Bot::claimed_by(const ObservedUnit &ally) const {
    // The camp an ally's published claim names, or -1.
    for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
        if (s != slot && ally.champion() == roster[s])
            return TeamMemory::objective(shared.get(s), groups.size());
    return -1;
}

bool Bot::capture_outranked(const CampGroup &g) const {
    // A visible ally on the capture that claims it holds it instead of us
    // when it outranks us (outranks()). Every holder ranks the same way, so
    // exactly one stays.
    const bool on = g.tiles().is_set(position);
    for (const auto &ally : allies) {
        if (ally.health <= 0 || !g.tiles().is_set(ally.position))
            continue;
        // Only an ally that claims the camp means to hold it; one crossing
        // the tiles or retreating over them leaves the next turn.
        // ... or, on the tiles while we are not, reports a fight (a holder
        // asking for help; a fighter on our own tiles is a passer-by).
        if (claimed_by(ally) != int(&g - groups.data()) && !(!on && reports_fight(ally)))
            continue;
        // One riding its capture's recall home cannot step off: it holds the
        // capture, and stepping on only halves its reward. Three in four of
        // our shared captures (19% of all, willow best girl's 2%) were one of
        // ours stepping onto a holder recalling with it.
        if (ally.channel || outranks(ally, !on))
            return true;
    }
    return false;
}

bool Bot::capture_deferred(const CampGroup &g) const {
    // A teammate claiming the group that rides its capture's recall home or
    // outranks us (outranks(): on the tiles, or within 3 with the bigger
    // bag) holds it from the start:
    // two on the tiles split the reward (a fifth of our captures, willow
    // best girl's 2%).
    const int group = int(&g - groups.data());
    for (const auto &ally : allies)
        if (ally.health > 0 && claimed_by(ally) == group && distance(ally.position, g.centre) <= 3 &&
            (ally.channel || (g.tiles().is_set(ally.position) ? outranks(ally, true) : ally.gems > int(gems))))
            return true;
    return false;
}

int Bot::capture_payout(const CampGroup &g) const {
    // The timer starts at the first capture phase held, no earlier than the
    // round after we stepped on, and pays capture_time phases later
    // (docs/GameRules.md: a five-round timer counted down by later phases).
    return std::max(g.holding_since, capture_since + 1) + rules::settings().capture_time;
}

bool Bot::outranks(const ObservedUnit &ally, bool arriving) const {
    // The holder order of capture_outranked(): the bigger bag (the Prism
    // carries the team's gems), then one already on the tiles, then the
    // lower id. willow best girl's holder is the first of the party on in
    // 84-86% of party captures; a type rank after the bag let our Vesper
    // hold over Marcella and the HoR (its capture gold 957 a game on the
    // same maps to willow's 468, the HoR's item health r300 172 to 290).
    return ally.gems != int(gems) ? ally.gems > int(gems) : arriving || id(ally.champion()) < id(bc::self());
}

bool Bot::home_after_capture(const CampGroup &g) const {
    // The holder of a capture goes home on the recall this turn ends in
    // (fake_recall()) instead of cancelling it. The capture pays at a round
    // start, five capture phases after its timer starts, and the recall
    // lands at our turn five turn ends after it starts, so starting it in
    // the round the timer starts lands it in the round of the reward, after
    // the capture phase that pays it, as willow best girl's champions do.
    // The timer starts at the first capture phase we hold through, and no
    // later than the round after we stepped on: the team's holding can be
    // older than ours (a teammate's that ended). A Home emblem's shorter
    // recall starts as many rounds later. Home when hurt (willow best girl
    // went home after 74-82% of captures below half health, 56-71% at
    // 50-80%, 23-35% above) or at the end of a short trip out
    // (policy::short_trip_rounds; not a champion that keeps its gems, which
    // has nothing to bank); never with an enemy near, whose hit would cancel
    // it (they stay out then).
    if (g.holding_since < 0 || incoming(position) > 0 ||
        int(round) < capture_payout(g) - recall_turns)
        return false;
    for (const auto *list : {&enemies, &ghosts})
        for (const auto &e : *list)
            if (distance(position, e.position) <= 8)
                return false;
    // Hurt: always home (a little less eagerly when behind).
    if (health < max_health * policy::home_capture_health * (1.0f - 0.5f * behind_pressure()))
        return true;
    // A neighbour worth more than the trip: stay out. Returning false makes
    // the next turn cancel this recall.
    if (chain_better_than_home(g))
        return false;
    // Otherwise the old fallbacks: a short trip out, or gold for a shard.
    const bool short_trip =
        passive != bc::passive_type::gem_focus && int(round) - left_spawn < policy::short_trip_rounds;
    return short_trip || bc::self().gold_count() >= unsigned(policy::min_shop_gold);
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

float Bot::behind_pressure() const {
    const float ours = float(bc::global_submitted_gems(bc::team::ally));
    const float theirs = float(bc::global_submitted_gems(bc::team::enemy));
    const float scale = std::max(40.0f, float(rules::settings().winning_gems) * 0.08f);
    const float ramp = std::min(1.0f, float(round) / float(policy::behind_ramp)); // early gaps are noise
    return std::clamp((theirs - ours) / scale, 0.0f, 1.0f) * ramp;
}

void Bot::build_hops() {
    hops.assign(groups.size(), {});
    for (std::size_t i = 0; i < groups.size(); ++i) {
        for (std::size_t j = 0; j < groups.size(); ++j) {
            if (i == j)
                continue;
            const int d = distance(groups[i].centre, groups[j].centre);
            if (d <= policy::hop_radius)
                hops[i].push_back({int(j), d});
        }
        std::sort(hops[i].begin(), hops[i].end(),
                  [](const auto &a, const auto &b) { return a.second < b.second; });
        if (hops[i].size() > 3)
            hops[i].resize(3);
    }
}

// decides when to go for another camp instead of just going home
bool Bot::chain_better_than_home(const CampGroup &g) const {
    const int gi = int(&g - groups.data());
    if (gi < 0 || std::size_t(gi) >= hops.size())
        return false;
    const float progress = std::min(1.0f, float(round) / float(std::max(1u, rules::settings().max_rounds / 2)));
    const float gem_w = gem_reward_weight(opening);
    const float gold_w =
        (policy::early_gold_value * (1 - progress) + policy::late_gold_value * progress) * gold_bonus;
    const float behind = behind_pressure();
    const float spare =
        std::max(0.0f, health - std::max(max_health * policy::casualty_margin, incoming(position) * 2));
    const float health_scale = std::max(100.0f, health);
    const float gold_now = float(bc::self().gold_count());

    float best_margin = 0;
    for (const auto &[j, hop] : hops[gi]) {
        const auto &n = groups[j];
        if (n.status == CampGroup::capture)
            continue;
        const float wait =
            n.status == CampGroup::cooling ? float(std::max(0, n.respawn - int(round) - hop)) : 0.0f;
        if (wait > float(policy::hop_max_wait))
            continue;
        bool monolith = false; // Team-only, as in the scorer.
        for (const auto &c : n.camps)
            monolith = monolith || c.stats.base_attack_range >= 5;
        if (monolith)
            continue;

        const float clear =
            std::max(1.0f, (n.status == CampGroup::cooling ? n.max_health : n.health) / dps(n));
        const float time_n = float(hop) + wait + clear + float(rules::settings().capture_time) + 1;
        const float reward = n.unit.gem_value() * gem_w + n.unit.gold_value() * gold_w;
        const float rate = reward / time_n;
        const float loss = clear_loss(n, clear);

        // Death odds: can we afford its damage, was an enemy seen near it, will
        // we finish low.
        float p = 0.02f + 1.2f * std::max(0.0f, loss - spare) / health_scale;
        if (std::size_t(j) < enemy_seen_near.size() &&
            int(round) - enemy_seen_near[j] <= 2 * policy::worker_sighting_memory)
            p += 0.15f;
        if (health - loss < 0.35f * max_health)
            p += 0.10f;
        p = std::clamp(p, 0.0f, 0.9f);

        // A death costs 60 rounds of income, the bag, and the 100-gold bounty.
        // Banked gold is not lost.
        const float death_cost =
            (60.0f * rate + float(gems) * gem_w + 100.0f * gold_w) * (1 - policy::behind_risk_relief * behind);
        // Unspent gold is delayed power: each affordable shard is worth ~4% income.
        const float delay = rate * std::min(0.5f, gold_now / 30.0f * policy::shard_income_gain) * time_n;
        // Chaining also saves the round trip home and back to this neighbour.
        const float trip = float(recall_turns + 1) + float(n.home_distance);
        const float margin = (reward + rate * trip) - (p * death_cost + delay);
        best_margin = std::max(best_margin, margin);
    }
    return best_margin > 0;
}
