// Worker camp choice and farming.
#include "../bot.h"
#include <tuple>
#include <wasm_simd128.h>

int Bot::choose_opening(int &until) const {
    // The team's opening: one group every champion clears together before
    // they spread, as the top teams open (all of Mite's and print dont
    // int's champions on the nearest big group, first hits at rounds
    // 13-31, in all 11 openings watched; ours spread from round 0). Each
    // champion works it out alike from what all share (the map, the
    // roster's starting kits, the walks from both spawns): a group the
    // whole team clears without a trip home, that we reach no later than
    // the enemy could, at the best reward a round of walk, clear and
    // capture; ore groups too (an opening limited to striking camps passed
    // a 343-gem centre the enemy then took). Groups as near their spawn as
    // ours were met by the enemy mid-clear; but print dont int's 34
    // openings are all on groups no nearer the enemy (4 exactly halfway,
    // guardians' elk pair among them), many done well after the enemy
    // could have walked there, and required done before the enemy could
    // walk there, ours opened on guardians' only 7-gem cluster with four
    // champions. Trips home planned in (camp choice's pricing) moved 35 of
    // 80 openings from wolves to elk groups and golems, first captures from
    // round 46 to 62 (one lost-world seat took no creature group all game).
    float rate = 0;
    for (const auto &k : roster_kits)
        rate += k.camp_rate;
    const float members = float(std::max<std::size_t>(1, roster_kits.size()));
    const float speed = std::max(1.0f, rate * policy::team_clear_share);
    const float gem_weight = gem_reward_weight(opening);
    const int capture_time = rules::settings().capture_time;
    int best = -1;
    float best_value = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto &g = groups[i];
        if (g.home_distance == bc::distance_field::unreachable || g.home_distance > g.enemy_distance)
            continue;
        if (g.status != CampGroup::alive || i == std::size_t(opening_last))
            continue; // A later opening group (strategy.h opening_groups): not the one just taken.
        if (centre_opening() && g.home_distance > opening_reach_now)
            continue;
        // Without a trip home: what each member spends before it leaves,
        // above the casualty margin and the walk out of the camps' reach
        // under their fire from its attack range (the leave rule's,
        // health_recovery_needed()). Counted down to the margin alone,
        // harvest's golem passed: the members left at the walk out (about
        // 480 by a golem) one by one, the golem stood at 1,367 then 506
        // health for 60 rounds, and our first capture came at round 133.
        float spare = 0;
        for (const auto &k : roster_kits) {
            const float exit = g.max_pressure * float(std::max(0, g.reach - (k.range - 1)));
            spare += std::max(0.0f, k.health - std::max(k.health * policy::casualty_margin, exit));
        }
        if (g.whole_work / speed > spare)
            continue;
        const float clear = std::max(g.max_health / speed, float(g.camps.size()) / members);
        const float done = float(g.home_distance) + clear + float(capture_time + 1);
        const float reward = g.unit.gem_value() * gem_weight + policy::early_gold_value * float(g.unit.gold_value());
        const float value = reward / done;
        if (value > best_value) {
            best_value = value;
            best = int(i);
            until = int(std::ceil(done));
        }
    }
    return best;
}

int Bot::worker_choose_group(int excluded) {
    // The opening group, while it stands: every champion's target until
    // its camps are down (its capture's holder then keeps it), it is gone
    // or left out, enemies are seen at it, or its planned time is past.
    if (!opening_done) {
        if (opening_group == -2)
            opening_group = choose_opening(opening_until);
        if (opening_group >= 0 && opening_group != excluded && int(round) <= opening_until &&
            (policy::opening_holds || !enemy_near_lately(opening_group)) &&
            groups[std::size_t(opening_group)].status == CampGroup::alive) {
            last_plan = round;
            siege_group = -1;
            cluster_run.clear();
            return opening_group;
        }
        // The next opening group, while the team still has openings to make
        // and the round allows (strategy.h opening_groups).
        if (policy::opening_groups > 1 && openings_made + 1 < policy::opening_groups && opening_group >= 0 &&
            int(round) < policy::opening_group_until && !enemy_near_lately(opening_group)) {
            ++openings_made;
            opening_last = opening_group;
            opening_group = choose_opening(opening_until);
            if (opening_group >= 0) {
                opening_until += int(round);
                last_plan = round;
                siege_group = -1;
                cluster_run.clear();
                return opening_group;
            }
        }
        opening_done = true;
    }
    // A group we were walled off from lately is no candidate.
    const int walled = int(round) < stuck_group_until ? stuck_group : -1;
    profiling::Scope scope(profiling::Stage::choose);
    measure_paths(1);
    const float gold_value = gold_value_now();
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
    static std::vector<int> claimed_groups;
    claimed_groups.clear();
    for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
        if (const int c = mate_claim(s); c >= 0) {
            ++claims[c];
            if constexpr (policy::cohesion_hazard > 0)
                claimed_groups.push_back(c);
        }
    // Each teammate's damage per round on camps (from its live strength and
    // focus in view, else its type's base) and, per camp, its claimants'
    // summed.
    std::array<float, TeamMemory::slots> mate_rate{};
    const unsigned kits = unsigned(std::min<std::size_t>(roster_kits.size(), TeamMemory::slots));
    for (unsigned s = 0; s < kits; ++s)
        mate_rate[s] = roster_kits[s].camp_rate;
    for (unsigned s = 0; s < kits; ++s)
        if (const int k = slot_ally[s]; k >= 0) {
            const auto &ally = allies[std::size_t(k)];
            const auto &kit = roster_kits[s];
            mate_rate[s] = camp_dps(kit.spells, ally.strength, ally.focus, 1) * kit.camp_multiplier;
        }
    claim_rates.assign(groups.size(), 0.0f);
    for (unsigned s = 0; s < kits; ++s)
        if (const int c = mate_claim(s); c >= 0)
            claim_rates[std::size_t(c)] += mate_rate[s];
    // Each teammate: where it is (in view, else at its claimed group, else
    // home), its health (in view, else its type's base), and what keeps it
    // busy before it could go elsewhere (the rest of a clear or capture it
    // is on, a recall). One in a fight, or on its way to one, is not free.
    struct Mate {
        vec2 where;
        float busy = 0, health = 0, max_health = 0;
        int claim = -1, range = 1;
        bool free = false;
    };
    std::array<Mate, TeamMemory::slots> mates{};
    for (unsigned s = 0; s < kits; ++s) {
        if (s == slot || !living(s))
            continue;
        auto &m = mates[s];
        const unsigned code = shared.get(s);
        m.claim = mate_claim(s);
        m.range = roster_kits[s].range;
        m.health = m.max_health = roster_kits[s].health;
        m.where = home_centre;
        m.free = code != TeamMemory::unavailable && TeamMemory::fight_group(code, groups.size()) < 0;
        if (m.claim >= 0) {
            const auto &h = groups[std::size_t(m.claim)];
            m.where = h.centre;
            if (h.status == CampGroup::capture)
                // Its holding's timer; unknown counts from now (capture_payout()
                // times only our own stepping on).
                m.busy = float(std::max(0, (h.holding_since >= 0 ? h.holding_since : int(round)) +
                                               int(rules::settings().capture_time) - int(round)));
            else if (h.status == CampGroup::alive && h.health < h.max_health)
                // At its claimants' speed together, not its own alone.
                m.busy = h.health / std::max({1.0f, mate_rate[s], claim_rates[std::size_t(m.claim)]}) +
                         float(rules::settings().capture_time + 1);
        } else if (code == TeamMemory::recall)
            m.busy = float(recall_turns);
        if (const int k = slot_ally[s]; k >= 0) {
            const auto &ally = allies[std::size_t(k)];
            m.where = ally.position;
            m.health = ally.health;
            m.max_health = ally.max_health;
        }
    }
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
    const float gem_weight = gem_reward_weight(opening);
    const float behind = behind_pressure();
    const float enemy_side_mul = policy::enemy_side_factor + (1.0f - policy::enemy_side_factor) * behind;
    const bool far_opening = centre_opening();
    const int capture_time = rules::settings().capture_time;
    // A group's helpers are the teammates claiming it, and those a party
    // plan asks for (its request): no teammate is expected unasked.
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
        // Fixed: masks for a gem reward and a group on the enemy's side; the
        // gem reward, the gold, the altar term, the camp count and the walk
        // from home.
        std::int32_t *gem, *enemy_side;
        float *gem_reward, *gold, *altar, *camp_count, *home, *reach;
        // This choice's state, with the group's shared facts (its camps'
        // longest reach, the strikers' work) and its claimants: their count,
        // their summed camp rate and their arrival (weighted by rate), and
        // the front among them (the camps strike the nearest: the members
        // with the shortest attack range): that range, what its members can
        // spend on a first visit and on each (above the leave line), and
        // their count. factor: the score's multipliers on the value we add.
        std::int32_t *status, *respawn, *path, *siege_open, *marks, *sighted;
        float *remaining, *splash, *pressure, *striker_work, *reach_share, *claims, *claim_rate, *claim_eta,
            *front_range, *front_first, *front_visit, *front_count, *scores, *factor, *belief, *cohesion;
        // A cluster group's run (gather_run): the reward and the time its
        // members add, and whether the run scored above the group alone;
        // whether the clear is planned as a spell-range siege.
        float *run_reward, *run_time;
        std::int32_t *run_won, *siege_won;
    };
    static Lanes lanes;
    if (lanes.padded != padded) {
        lanes.padded = padded;
        lanes.ints.reset(new std::int32_t[10 * padded]);
        lanes.floats.reset(new float[24 * padded]);
        std::int32_t *next_int = lanes.ints.get();
        for (auto **lane : {&lanes.gem, &lanes.enemy_side, &lanes.status, &lanes.respawn, &lanes.path,
                            &lanes.siege_open, &lanes.marks, &lanes.sighted, &lanes.run_won, &lanes.siege_won}) {
            *lane = next_int;
            next_int += padded;
        }
        float *next_float = lanes.floats.get();
        for (auto **lane : {&lanes.gem_reward, &lanes.gold, &lanes.altar, &lanes.remaining, &lanes.splash,
                            &lanes.pressure, &lanes.claims, &lanes.scores, &lanes.striker_work, &lanes.reach_share,
                            &lanes.claim_rate, &lanes.claim_eta, &lanes.front_range, &lanes.front_first,
                            &lanes.front_visit, &lanes.front_count, &lanes.factor, &lanes.camp_count, &lanes.home,
                            &lanes.reach, &lanes.run_reward, &lanes.run_time, &lanes.belief, &lanes.cohesion}) {
            *lane = next_float;
            next_float += padded;
        }
        for (std::size_t i = count; i < padded; ++i) {
            for (auto *lane : {lanes.gem, lanes.enemy_side, lanes.status, lanes.respawn, lanes.siege_open,
                               lanes.marks})
                lane[i] = 0;
            for (auto *lane : {lanes.gem_reward, lanes.gold, lanes.altar, lanes.remaining, lanes.splash,
                               lanes.pressure, lanes.claims, lanes.striker_work, lanes.reach_share, lanes.claim_rate,
                               lanes.claim_eta, lanes.front_range, lanes.front_first, lanes.front_visit,
                               lanes.front_count, lanes.factor, lanes.camp_count, lanes.home, lanes.reach,
                               lanes.run_reward, lanes.run_time, lanes.belief, lanes.cohesion})
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
            lanes.gem_reward[i] = gem_value * gem_weight;
            lanes.gold[i] = float(g.unit.gold_value());
            lanes.camp_count[i] = float(g.camps.size());
            lanes.home[i] = float(g.home_distance);
            // The farthest two camps apart (Chebyshev): the larger of the
            // spans along the two axes, from one pass rather than every pair.
            int low_x = std::numeric_limits<int>::max(), high_x = std::numeric_limits<int>::min();
            int low_y = low_x, high_y = high_x;
            for (const auto &c : g.camps) {
                low_x = std::min(low_x, c.position.x);
                high_x = std::max(high_x, c.position.x);
                low_y = std::min(low_y, c.position.y);
                high_y = std::max(high_y, c.position.y);
            }
            const int extent = g.camps.empty() ? 0 : std::max(high_x - low_x, high_y - low_y);
            // The altar term, and the walk across a group longer than two
            // attack ranges: a row of 30 gold clusters scored as one quick
            // clear and walked ours along it into the enemy's half.
            // A gem group's walk counts the more for fewer altars
            // (policy::scarce_altar_weight); gold needs no tribute.
            const float altar_weight =
                0.25f + (gem_value > 0
                             ? policy::scarce_altar_weight / float(std::max<std::size_t>(1, altars.size()))
                             : 0.0f);
            lanes.altar[i] = float(std::min(policy::altar_walk_cap, g.altar_distance)) * altar_weight +
                             float(std::max(0, extent - 2 * range));
        }
    }
    // With a crystal, a group's walk is its hop's where that saves more
    // rounds than the crystal's price (hop_threshold()): from spawn its route
    // from there, on the way the plan's rounds left (planned so).
    const bool hopping = carrying_crystal();
    const bool on_spawn = hopping && hops_ready && home_field.get(position) == 0;
    const float threshold = hopping ? hop_threshold() : 0.0f;
    const auto hop_walk = [&](std::size_t i, int walk) {
        if (!hopping)
            return walk;
        if (on_spawn && float(walk - group_hops[i]) > threshold)
            walk = group_hops[i];
        int planned = hop_rounds(groups[i].centre);
        // The approach plans its hop toward the nearest camp, not the
        // centre (strategy.h hop_home): off spawn, a group chosen for its
        // hop read as its walk again and was dropped a step later.
        if constexpr (policy::hop_home)
            for (const auto &c : groups[i].camps)
                planned = std::min(planned, hop_rounds(c.position));
        return std::min(walk, planned);
    };
    // Teammates' claims since our last choice: one that changed has ended
    // (claim_ended: the round we saw it end).
    static std::vector<int> claim_ended;
    static std::array<int, TeamMemory::slots> last_claim = [] {
        std::array<int, TeamMemory::slots> none{};
        none.fill(-1);
        return none;
    }();
    if (claim_ended.size() != groups.size())
        claim_ended.assign(groups.size(), std::numeric_limits<int>::min());
    for (unsigned s = 0; s < TeamMemory::slots; ++s) {
        const int c = s < roster.size() ? mate_claim(s) : -1;
        if (last_claim[s] >= 0 && last_claim[s] != c && std::size_t(last_claim[s]) < claim_ended.size())
            claim_ended[std::size_t(last_claim[s])] = int(round);
        last_claim[s] = c;
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
            path[i] = hop_walk(i, paths[i]);
            siege[i] = range > 1 && g.siege_open > 0 ? -1 : 0;
            mark[i] = marks[i];
            // The group's shared facts (CampGroup::refresh_live()): of its
            // living camps while it lives, else of the whole group (a capture
            // has no living camps, and was read as no hard group, reach 0).
            const bool whole = g.status != CampGroup::alive;
            remaining[i] = whole ? g.max_health : g.health;
            lanes.striker_work[i] = whole ? g.whole_work : g.live_work;
            lanes.reach_share[i] = g.reach_share;
            lanes.reach[i] = float(whole ? g.reach : g.live_reach);
            // Tiles past the camp spacing to the nearest teammate's claim
            // (none worked out while the penalty is off).
            lanes.cohesion[i] = 0.0f;
            if (policy::cohesion_hazard > 0 && opening.spacing <= policy::cohesion_max_spacing &&
                !(whole ? g.hard : g.live_hard)) {
                float nearest_claim = std::numeric_limits<float>::infinity();
                for (const int claimed : claimed_groups)
                    nearest_claim = std::min(nearest_claim, float(distance(g.centre, groups[claimed].centre)));
                if (std::isfinite(nearest_claim))
                    lanes.cohesion[i] = std::max(0.0f, nearest_claim - opening.spacing);
            }
            splash[i] = g.splash;
            pressure[i] = g.pressure;
            claim[i] = float(claims[i]);
        }
        for (std::size_t i = 0; i < count; ++i)
            lanes.sighted[i] = i < enemy_seen_near.size() &&
                                       int(round) - enemy_seen_near[i] <= policy::worker_sighting_memory
                                   ? -1
                                   : 0;
        // A group out of view, last seen alive before a teammate's claim on
        // it ended, is taken with policy::claimed_dead_share until its respawn
        // would be past (counted from the claim's end).
        for (std::size_t i = 0; i < count; ++i) {
            const auto &g = groups[i];
            const bool doubtful = g.status == CampGroup::alive && g.seen < int(round) && claim_ended[i] > g.seen &&
                                  int(round) - claim_ended[i] < g.unit.max_respawn_timer() + capture_time + 1;
            lanes.belief[i] = doubtful ? 1.0f - policy::claimed_dead_share : 1.0f;
        }
        // The claimants, as one party: their summed camp rate, arriving at
        // their rate-weighted walk (walk_bound(): from where each is in view,
        // else from the group, to within the camps' reach and 3, where
        // worker_farm() strikes from), and their front.
        for (std::size_t i = 0; i < count; ++i) {
            lanes.claim_rate[i] = claim_rates[i];
            lanes.claim_eta[i] = 0;
            lanes.front_range[i] = float(bc::distance_field::unreachable);
            lanes.front_first[i] = lanes.front_visit[i] = lanes.front_count[i] = 0;
        }
        for (unsigned s = 0; s < kits; ++s) {
            const auto &m = mates[s];
            if (m.claim < 0)
                continue;
            const std::size_t i = std::size_t(m.claim);
            const vec2 from = slot_ally[s] >= 0 ? m.where : groups[i].centre;
            // Out of view, a claimant is taken to be at its group (its centre
            // can be a wall: no walk at all).
            const int walk = walk_bound(from, int(i));
            const float arrival = walk == bc::distance_field::unreachable
                                      ? 0.0f
                                      : std::max(0.0f, float(walk) - (lanes.reach[i] + 3.0f));
            if (claim_rates[i] > 0)
                lanes.claim_eta[i] += arrival * mate_rate[s] / claim_rates[i];
            const float mate_range = float(m.range);
            if (mate_range < lanes.front_range[i]) {
                lanes.front_range[i] = mate_range;
                lanes.front_first[i] = lanes.front_visit[i] = lanes.front_count[i] = 0;
            }
            if (mate_range == lanes.front_range[i]) {
                lanes.front_first[i] += policy::camp_visit_health(m.health, m.max_health);
                lanes.front_visit[i] += m.max_health;
                lanes.front_count[i] += 1;
            }
        }
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
    const float gold_weight = gold_value * gold_bonus;
    // Marcella walks to the bigger groups (policy::marcella_travel_hazard).
    const float travel_hazard =
        type == bc::champion_type::marcella ? policy::marcella_travel_hazard : policy::travel_hazard;
    // Multi-visit clears: the health a visit can spend before going home
    // (at a camp, all but the walk out: health_recovery_needed()), from
    // home whole and now only above the line we go home at, the rounds
    // left, and a trip home's recall.
    const float usable = max_health, first_visit = policy::camp_visit_health(health, max_health);
    const float rounds_left = float(int(rules::settings().max_rounds) - int(round));
    const float recall_trip = float(recall_turns + 1);
    // The members the scoring reads, copied: its stores to the lanes could
    // alias the members themselves, so each would be read again (and the
    // terms made from it worked out again) for every four groups.
    const int our_range = range, now = int(round), current = target;
    const float our_focus = focus, our_multiplier = camp_multiplier, camp_scale = camp_mitigation * mitigation;
    const std::array<Spell, 2> kit = spells;
    // Cluster runs. A short trip's capture ends in a recall home
    // (home_after_capture()), which every version that kept farming instead
    // lost to; but a row of gem or gold clusters a tile or
    // two apart (each its own group and capture) was then taken one per
    // trip. A cluster group's run is the clusters taken after it on the same
    // trip: next, the nearest cluster group linked to the last member
    // (cluster_links_of()) that is not in the run and is open on arrival
    // (cluster_open()), while the capture that pays it still ends in the
    // short trip's recall: the recall starts recall_turns before the
    // payout, within short_trip_rounds of leaving spawn. Only a champion
    // that recalls after a short trip, during one, plans runs.
    const bool runs = now - left_spawn < policy::short_trip_rounds;
    const float run_deadline = float(left_spawn + policy::short_trip_rounds + recall_turns);
    // Teammates' claims (each one's objective), and the groups in the run
    // being planned, marked with its stamp.
    static std::vector<std::uint8_t> taken;
    static std::vector<std::uint32_t> run_mark;
    static std::uint32_t run_stamp = 0;
    if (runs) {
        link_clusters();
        taken.assign(count, 0);
        for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
            if (const int claimed = mate_claim(s); claimed >= 0)
                taken[std::size_t(claimed)] = 1;
        run_mark.resize(count);
    }
    // A cluster group's clear as score_lanes() works it out with nobody
    // claiming it (a run member is unclaimed).
    const auto cluster_clear = [&](int k) {
        const auto &g = groups[k];
        float rate = flat_dps;
        for (const auto &spell : kit)
            if (spell.kind == bc::ability_kind::aoe_damage)
                rate += our_focus * spell.modifier * (g.splash - 1) / float(spell.cooldown);
        rate += dash_rate * std::min(2.0f, float(g.camps.size()));
        const float remaining = g.status == CampGroup::cooling ? g.max_health : g.health;
        return std::max(remaining / std::max(1.0f, rate * our_multiplier * policy::team_clear_share),
                        float(g.camps.size()));
    };
    // Group k's run: the reward and time its members add (none without a
    // run), and with `members` the members after k in order. Each member
    // costs the walk onto its tiles from the last one's, its clear and its
    // capture; the first capture pays after the walk onto the group's tiles
    // (or its respawn), its clear and its capture.
    const auto gather_run = [&](std::size_t k, std::vector<int> *members) {
        float extra_reward = 0, extra_time = 0;
        const auto &g = groups[k];
        if (runs && cluster_group[k] && lanes.path[k] != bc::distance_field::unreachable) {
            const float walk = float(lanes.path[k]) * travel_scale;
            float finish = float(now) + capture_time + 1 +
                           (g.status == CampGroup::cooling ? std::max(walk, float(g.respawn - now)) : walk) +
                           (g.status == CampGroup::capture ? 0.0f : cluster_clear(int(k)));
            const float first = finish;
            run_mark[k] = ++run_stamp;
            // A member takes at least a capture and a round more.
            for (int last = int(k); finish + float(capture_time + 1) < run_deadline;) {
                // The last member's links are nearest first: the first open
                // one is the next.
                int next = -1;
                float arrival = 0;
                for (const auto &link : cluster_links_of(last)) {
                    if (link.apart > capture_time)
                        break;
                    arrival = finish + float(link.apart) * travel_scale;
                    if (run_mark[link.group] != run_stamp && link.group != walled &&
                        cluster_open(link.group, int(arrival), taken[link.group])) {
                        next = link.group;
                        break;
                    }
                }
                if (next < 0)
                    break;
                const float done = arrival + cluster_clear(next) + capture_time + 1;
                if (done >= run_deadline)
                    break;
                run_mark[next] = run_stamp;
                if (members)
                    members->push_back(next);
                extra_reward += lanes.gem_reward[next] + gold_weight * lanes.gold[next];
                finish = done;
                last = next;
            }
            extra_time = finish - first;
        }
        lanes.run_reward[k] = extra_reward;
        lanes.run_time[k] = extra_time;
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
            // An enemy capture of a group of more than contest_max_camps
            // camps is not contested (strategy.h): a 30-cluster road.
            if constexpr (policy::contest_max_camps > 0)
                skip = wasm_v128_or(skip, wasm_v128_and(enemy_here, wasm_f32x4_gt(wasm_v128_load(&lanes.camp_count[i]),
                                                                                  splat(float(policy::contest_max_camps)))));
            v128_t travel_steps = wasm_i32x4_max(wasm_i32x4_sub(path, splat_int(our_range)), no_bits);
            travel_steps = pick(path, travel_steps, is_held); // Holding means standing on a tile.
            const v128_t splash = wasm_v128_load(&lanes.splash[i]),
                         remaining = wasm_v128_load(&lanes.remaining[i]);
            const v128_t pressure = wasm_v128_load(&lanes.pressure[i]),
                         claimants = wasm_v128_load(&lanes.claims[i]);
            // A capture a teammate claims is its holder's, unless we hold it:
            // walking there only finds the tile taken.
            skip = wasm_v128_or(skip, wasm_v128_andnot(wasm_v128_and(is_held, wasm_f32x4_gt(claimants, zero)),
                                                       is_here));
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
            // Clearing speeds (damage a round on the group): ours and the
            // claimants' summed, each at team_clear_share of its rate.
            const v128_t speed = wasm_f32x4_pmax(
                one, wasm_f32x4_mul(wasm_f32x4_mul(rate, splat(our_multiplier)), splat(policy::team_clear_share)));
            const v128_t claim_speed =
                wasm_f32x4_mul(wasm_v128_load(&lanes.claim_rate[i]), splat(policy::team_clear_share));
            const v128_t claimed = wasm_f32x4_gt(claim_speed, zero);
            // A champion with a dash travels the faster, and so waits the
            // longer for a respawn.
            const v128_t travel = wasm_f32x4_mul(wasm_f32x4_convert_i32x4(travel_steps), splat(travel_scale));
            const v128_t respawn_in = wasm_f32x4_convert_i32x4(
                wasm_i32x4_sub(wasm_v128_load(&lanes.respawn[i]), splat_int(now)));
            // Each party strikes from its arrival, a cooling group from its
            // respawn.
            const v128_t start = wasm_v128_and(is_cooling, wasm_f32x4_pmax(zero, respawn_in));
            const v128_t ours_at = wasm_f32x4_pmax(travel, start);
            const v128_t theirs_at =
                pick(wasm_f32x4_pmax(wasm_v128_load(&lanes.claim_eta[i]), start), ours_at, claimed);
            const v128_t health_left = pick(zero, remaining, is_held);
            const v128_t camps_left = pick(zero, camps_in_group, is_held);
            // The clear with us: the party there first strikes alone until the
            // other arrives, then both. Planned from the last arrival, a
            // joiner on a group already under way scored as slowing it.
            const v128_t first_at = wasm_f32x4_pmin(ours_at, theirs_at),
                         later_at = wasm_f32x4_pmax(ours_at, theirs_at);
            const v128_t first_speed =
                pick(speed, wasm_f32x4_pmax(one, claim_speed), wasm_f32x4_le(ours_at, theirs_at));
            const v128_t both = wasm_f32x4_add(speed, claim_speed);
            const v128_t head = wasm_f32x4_mul(first_speed, wasm_f32x4_sub(later_at, first_at));
            v128_t end_with = pick(wasm_f32x4_add(first_at, wasm_f32x4_div(health_left, first_speed)),
                                   wasm_f32x4_add(later_at, wasm_f32x4_div(wasm_f32x4_sub(health_left, head), both)),
                                   wasm_f32x4_le(health_left, head));
            // Each camp takes at least one hit.
            end_with = wasm_f32x4_pmax(
                end_with, wasm_f32x4_add(first_at, wasm_f32x4_div(camps_left, wasm_f32x4_add(one, claimants))));
            v128_t end_without =
                wasm_f32x4_add(theirs_at, wasm_f32x4_div(health_left, wasm_f32x4_pmax(one, claim_speed)));
            end_without = wasm_f32x4_pmax(
                end_without, wasm_f32x4_add(theirs_at, wasm_f32x4_div(camps_left, wasm_f32x4_pmax(one, claimants))));
            // The clear's camp damage: the strikers' work at the clearing
            // speed, in rounds of a striker's strength; ours alone, only the
            // strikers that reach where we strike from (CampGroup::reach_share).
            const v128_t work = pick(zero, wasm_v128_load(&lanes.striker_work[i]), is_held);
            const v128_t front_range = wasm_v128_load(&lanes.front_range[i]),
                         front_count = wasm_v128_load(&lanes.front_count[i]);
            const v128_t we_front = wasm_f32x4_le(splat(float(our_range)), front_range);
            const v128_t we_alone = wasm_f32x4_lt(splat(float(our_range)), front_range);
            // Our mitigation counts while we are in front (the claimants'
            // own is not known out of view).
            const v128_t damage_with =
                wasm_f32x4_mul(wasm_f32x4_mul(wasm_f32x4_div(work, both), pick(splat(camp_scale), one, we_front)),
                               pick(one, wasm_v128_load(&lanes.reach_share[i]), claimed));
            const v128_t damage_without = wasm_f32x4_div(work, wasm_f32x4_pmax(one, claim_speed));
            // The camps strike the nearest: the front (the shortest attack
            // range) pays for the clear, the rest stand behind it. Each visit
            // ends with a walk out of the camps' reach under their fire (a
            // hit cancels the recall), from the front's attack range; the
            // first spends what we have if we are above the line we go home
            // at (camp_visit_health()), every later one a whole health. The
            // damage past that is paid in trips home (a recall and the walk
            // back), the fronts taking turns. A visit that cannot afford the walk out is no plan
            // (alone beside a monolith pair, whose reach of 5 outranges us
            // all, ours died within 3 rounds).
            const v128_t reach = wasm_v128_load(&lanes.reach[i]);
            const v128_t exit_cost = wasm_f32x4_mul(
                wasm_f32x4_mul(pressure, wasm_f32x4_pmax(zero, wasm_f32x4_sub(reach, splat(float(our_range) - 1)))),
                splat(camp_scale));
            const v128_t mates_exit = wasm_f32x4_mul(
                pressure, wasm_f32x4_pmax(zero, wasm_f32x4_sub(reach, wasm_f32x4_sub(front_range, one))));
            const v128_t mates_first = wasm_f32x4_pmax(
                zero, wasm_f32x4_sub(wasm_v128_load(&lanes.front_first[i]), wasm_f32x4_mul(front_count, mates_exit)));
            const v128_t mates_visit =
                wasm_f32x4_sub(wasm_v128_load(&lanes.front_visit[i]), wasm_f32x4_mul(front_count, mates_exit));
            // Our shields, in front: one absorbs at most the camp damage
            // arriving while it lasts.
            const v128_t our_clear = wasm_f32x4_pmax(one, wasm_f32x4_sub(end_with, ours_at));
            const v128_t per_round = wasm_f32x4_div(damage_with, our_clear);
            v128_t saved = zero;
            for (const auto &spell : kit)
                if (spell.kind == bc::ability_kind::shield)
                    saved = wasm_f32x4_add(
                        saved,
                        wasm_f32x4_mul(wasm_f32x4_pmin(splat(our_focus * spell.modifier),
                                                       wasm_f32x4_mul(per_round, splat(float(std::max(1, spell.duration))))),
                                       wasm_f32x4_add(one, wasm_f32x4_div(our_clear, splat(float(spell.cooldown))))));
            const v128_t ours_first =
                wasm_f32x4_add(wasm_f32x4_pmax(zero, wasm_f32x4_sub(splat(first_visit), exit_cost)), saved);
            const v128_t ours_visit = wasm_f32x4_sub(splat(usable), exit_cost);
            const v128_t first_with = wasm_f32x4_add(pick(zero, mates_first, we_alone), pick(ours_first, zero, we_front));
            const v128_t visit_with = wasm_f32x4_add(pick(zero, mates_visit, we_alone), pick(ours_visit, zero, we_front));
            const v128_t fronts_with = wasm_f32x4_add(pick(zero, front_count, we_alone), pick(one, zero, we_front));
            const v128_t each_with = wasm_f32x4_div(visit_with, wasm_f32x4_pmax(one, fronts_with));
            const v128_t each_without = wasm_f32x4_div(mates_visit, wasm_f32x4_pmax(one, front_count));
            const v128_t trips_with =
                wasm_f32x4_ceil(wasm_f32x4_div(wasm_f32x4_pmax(zero, wasm_f32x4_sub(damage_with, first_with)),
                                               wasm_f32x4_pmax(splat(100.0f), each_with)));
            const v128_t trips_without =
                wasm_f32x4_ceil(wasm_f32x4_div(wasm_f32x4_pmax(zero, wasm_f32x4_sub(damage_without, mates_first)),
                                               wasm_f32x4_pmax(splat(100.0f), each_without)));
            const v128_t trip_time =
                wasm_f32x4_add(splat(recall_trip), wasm_f32x4_mul(wasm_v128_load(&lanes.home[i]), splat(travel_scale)));
            const v128_t settle = wasm_f32x4_add(splat(float(capture_time + 1)), wasm_v128_load(&lanes.altar[i]));
            v128_t time_with = wasm_f32x4_add(
                wasm_f32x4_add(end_with,
                               wasm_f32x4_div(wasm_f32x4_mul(trips_with, trip_time), wasm_f32x4_pmax(one, fronts_with))),
                settle);
            const v128_t time_without = wasm_f32x4_add(
                wasm_f32x4_add(end_without,
                               wasm_f32x4_div(wasm_f32x4_mul(trips_without, trip_time), wasm_f32x4_pmax(one, front_count))),
                settle);
            v128_t ranged_dps = zero;
            for (const auto &spell : kit) {
                const bool area = spell.kind == bc::ability_kind::aoe_damage;
                // Area spells, and the targeted spells that can target a camp.
                if (siege_spell(spell))
                    ranged_dps = wasm_f32x4_add(
                        ranged_dps,
                        wasm_f32x4_mul(splat(our_focus * spell.modifier / spell.cooldown), area ? splash : one));
            }
            // siege_open is prepared in the background; unknown counts as
            // closed. A slower but lossless range-four clear, alone, where the
            // terrain leaves a tile out of every camp's reach: taken when it
            // ends before the clear with its trips home.
            const v128_t siege_time = wasm_f32x4_add(
                wasm_f32x4_add(ours_at, wasm_f32x4_div(health_left, wasm_f32x4_mul(ranged_dps, splat(our_multiplier)))),
                settle);
            const v128_t sieging = wasm_v128_and(
                wasm_v128_and(wasm_v128_and(wasm_f32x4_gt(ranged_dps, zero), wasm_v128_load(&lanes.siege_open[i])),
                              wasm_v128_andnot(wasm_f32x4_gt(trips_with, zero), claimed)),
                wasm_f32x4_lt(siege_time, time_with));
            wasm_v128_store(&lanes.siege_won[i], sieging);
            time_with = pick(siege_time, time_with, sieging);
            // Every camp counts, and on a fortress-gem map the gem half of the
            // reward dominates: a gold cluster pays 0 gems, so no amount of gold
            // route play converts it into a win.
            const v128_t reward =
                wasm_f32x4_add(wasm_v128_load(&lanes.gem_reward[i]),
                               wasm_f32x4_mul(splat(gold_weight), wasm_v128_load(&lanes.gold[i])));
            // The group's reward a round, discounted by the plan's length
            // (plan_hazard); ours is what we add to it: with us, less
            // without. Valued at its whole reward a round, a group drew every
            // claimant as if it cleared it alone (a crowding cost then sent
            // the fourth and fifth elsewhere), and a big group, worth little
            // to one champion, was never begun.
            // A group found taken on arrival ends the plan there: its reward
            // counts with the chance it is there (belief), its time is the
            // walk and, if it is, the rest.
            const v128_t belief = wasm_v128_load(&lanes.belief[i]);
            const auto believed = [&](v128_t time, v128_t walk) {
                walk = wasm_f32x4_pmin(walk, time);
                return wasm_f32x4_add(walk, wasm_f32x4_mul(belief, wasm_f32x4_sub(time, walk)));
            };
            const v128_t kept = wasm_f32x4_mul(belief, reward);
            const v128_t time_kept = believed(time_with, travel);
            const v128_t with_value =
                wasm_f32x4_div(wasm_f32x4_div(kept, wasm_f32x4_pmax(one, time_kept)),
                               wasm_f32x4_add(one, wasm_f32x4_mul(time_kept, splat(policy::plan_hazard))));
            const v128_t time_kept_without = believed(time_without, wasm_v128_load(&lanes.claim_eta[i]));
            const v128_t without_value = wasm_v128_and(
                claimed, wasm_f32x4_div(wasm_f32x4_div(kept, wasm_f32x4_pmax(one, time_kept_without)),
                                        wasm_f32x4_add(one, wasm_f32x4_mul(time_kept_without, splat(policy::plan_hazard)))));
            v128_t value = wasm_f32x4_sub(with_value, without_value);
            value = pick(zero, value, wasm_f32x4_gt(time_with, splat(rounds_left)));
            // A cluster group's run (gather_run) is one plan: the group's
            // reward and time with its members', under the same hazard. The
            // better of the two stands; with no run it is the group's own.
            {
                const v128_t run_duration = wasm_f32x4_add(time_with, wasm_v128_load(&lanes.run_time[i]));
                const v128_t run_kept = believed(run_duration, travel);
                v128_t run = wasm_f32x4_div(
                    wasm_f32x4_mul(belief, wasm_f32x4_add(reward, wasm_v128_load(&lanes.run_reward[i]))),
                    wasm_f32x4_pmax(one, run_kept));
                run = wasm_f32x4_div(run, wasm_f32x4_add(one, wasm_f32x4_mul(run_kept, splat(policy::plan_hazard))));
                run = pick(splat(skipped), run, wasm_v128_or(wasm_f32x4_gt(run_duration, splat(rounds_left)), claimed));
                wasm_v128_store(&lanes.run_won[i], wasm_f32x4_gt(run, value));
                value = wasm_f32x4_pmax(value, run);
            }
            // No visit affords the walk out: no plan at all.
            skip = wasm_v128_or(skip, wasm_v128_andnot(wasm_f32x4_lt(each_with, splat(100.0f)), is_held));
            // The score's multipliers, kept for the party plans.
            v128_t factor = wasm_f32x4_div(one, wasm_f32x4_add(one, wasm_f32x4_mul(ours_at, splat(travel_hazard))));
            if constexpr (policy::cohesion_hazard > 0)
                factor = wasm_f32x4_div(
                    factor, wasm_f32x4_add(one, wasm_f32x4_mul(wasm_v128_load(&lanes.cohesion[i]),
                                                               splat(policy::cohesion_hazard))));
            factor = pick(wasm_f32x4_mul(factor, splat(enemy_side_mul)), factor, wasm_v128_load(&lanes.enemy_side[i]));
            // A centre map's opening: our own side first (policy::opening_rounds).
            if (far_opening)
                factor = pick(wasm_f32x4_mul(factor, splat(policy::opening_far_mul)), factor,
                              wasm_f32x4_gt(wasm_v128_load(&lanes.home[i]), splat(float(opening_reach_now))));
            factor = pick(wasm_f32x4_mul(factor, splat(policy::enemy_held_camp_bonus)), factor, enemy_here);
            // Camps by a stronger group we broke contact with wait out the
            // sighting (worker_sighting_memory).
            factor = pick(wasm_f32x4_mul(factor, splat(policy::worker_sighting_penalty)), factor,
                          wasm_v128_load(&lanes.sighted[i]));
            // While the squad is depleted, converge on the allies still on
            // the map instead of spreading out and feeding into a grouped
            // enemy one at a time.
            if (depleted)
                factor = pick(wasm_f32x4_mul(factor, splat(policy::depleted_rally_bonus)), factor,
                              wasm_f32x4_gt(claimants, zero));
            factor = pick(wasm_f32x4_mul(factor, splat(policy::worker_commit_bonus)), factor,
                          wasm_i32x4_eq(index, splat_int(current)));
            factor = pick(wasm_f32x4_mul(factor, splat(policy::held_capture_bonus)), factor, wasm_v128_and(is_held, is_here));
            wasm_v128_store(&lanes.factor[i], factor);
            wasm_v128_store(&lanes.scores[i], pick(splat(skipped), wasm_f32x4_mul(value, factor), skip));
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
    for (std::size_t i = 0; i < count; ++i)
        gather_run(i, nullptr);
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
                if (lanes.path[k] != hop_walk(k, group_path[k])) {
                    lanes.path[k] = hop_walk(k, group_path[k]);
                    gather_run(k, nullptr); // Its run starts from the new walk.
                    changed = true;
                }
            if (changed)
                score_lanes(i);
        }
    }
    // Party plans: a group we cannot clear alone without trips home, with
    // the teammates who could be there soonest asked to come (request_group,
    // answered by those who then choose it). Print dont int starts its elk
    // groups with 3-4 champions at base stats, and its golems and monoliths
    // with the team; valued alone, those groups never win, and valued with
    // the claimants only, nobody begins them. A plan counts what it adds
    // with the k soonest teammates (each from what keeps it busy, then its
    // walk; one in a fight is not free), less what they would add elsewhere:
    // our best score at their camp rate (we cannot score their choices).
    // Planned offline on print dont int's 34 case games, this took 16 of 22
    // near multi-elk groups by round 400 (claimants only 10), parties of
    // 3.0 early and 2.1 from round 400 (print 3.2-4.0, 2.3).
    if constexpr (policy::diagnostic) {
        std::printf("CHOICE r=%u id=%u pos=%d,%d choice=%d best=%.2f spawn=%d crystal=%d |", round, id(bc::self()),
                    position.x, position.y, choice, best, int(home_field.get(position) == 0), int(hopping));
        for (std::size_t i = 0; i < count; ++i)
            if (groups[i].unit.gem_value() >= 25 || groups[i].unit.gold_value() >= 300)
                std::printf(" g%zu:s=%.2f p=%d st=%d", i, lanes.scores[i], lanes.path[i], int(groups[i].status));
        std::printf("\n");
    }
    int party = -1;
    if (choice >= 0 && best > 0) {
        struct Cohort {
            float at, speed;
        };
        const float our_rate = flat_dps * our_multiplier;
        const float party_best = best;
        // A plan asks one free teammate at least: what the slowest of them
        // would add elsewhere is the least any plan gives up (none free: no
        // plan).
        float least_asked = std::numeric_limits<float>::infinity();
        for (unsigned s = 0; s < kits; ++s)
            if (s != slot && living(s) && mates[s].free)
                least_asked = std::min(least_asked, mate_rate[s]);
        const float least_given_up = party_best * least_asked / our_rate * policy::party_ask_discount;
        // And the fastest a party can clear: every free teammate striking.
        float all_mates_speed = 0;
        for (unsigned s = 0; s < kits; ++s)
            if (s != slot && living(s) && mates[s].free)
                all_mates_speed += mate_rate[s] * policy::team_clear_share;
        for (std::size_t i = 0; i < count && least_asked != std::numeric_limits<float>::infinity(); ++i) {
            const auto &g = groups[i];
            // Groups whose walk is measured, with a request code to ask with.
            const bool cluster_party = policy::party_cluster_camps > 0 && lanes.striker_work[i] <= 0 &&
                                       lanes.camp_count[i] >= float(policy::party_cluster_camps);
            if (lanes.scores[i] == skipped || group_bounded[i] || g.status == CampGroup::capture ||
                (lanes.striker_work[i] <= 0 && !cluster_party) || !shared.camp_request(int(i)))
                continue;
            float rate = flat_dps;
            for (const auto &spell : kit)
                if (spell.kind == bc::ability_kind::aoe_damage)
                    rate += our_focus * spell.modifier * (lanes.splash[i] - 1) / float(spell.cooldown);
            rate += dash_rate * std::min(2.0f, lanes.camp_count[i]);
            const float speed = std::max(1.0f, rate * our_multiplier * policy::team_clear_share);
            const float start = g.status == CampGroup::cooling ? std::max(0.0f, float(g.respawn - now)) : 0.0f;
            const float settle = float(capture_time + 1) + lanes.altar[i];
            const float reward = lanes.gem_reward[i] + gold_weight * lanes.gold[i];
            const float our_walk = float(std::max(0, lanes.path[i] - our_range)) * travel_scale;
            const float belief = lanes.belief[i];
            const auto plan_value = [&](float time) {
                const float walk = std::min(our_walk, time);
                time = walk + belief * (time - walk);
                return belief * reward / std::max(1.0f, time) / (1 + time * policy::plan_hazard);
            };
            const float claim_speed = lanes.claim_rate[i] * policy::team_clear_share;
            // No plan here ends its clear before `start` and a round a camp
            // shared by everyone who could come, nor before its health at
            // all of them striking (less a rounding margin), nor settles
            // sooner; a plan's value only falls with its time and what it
            // asks: a group whose value then, less the least a plan gives
            // up, cannot beat the best is passed before the teammates' walks
            // (they were 24k of a choice's 41k fuel, the turns past 100k
            // freezing champions a round each; 24-32k still on lanes and
            // boss-battle with the camp count alone).
            const float soonest_clear =
                start + std::max(lanes.camp_count[i] / (lanes.claims[i] + 1 + float(kits)),
                                 0.999f * lanes.remaining[i] / (speed + claim_speed + all_mates_speed) - 0.01f);
            if (lanes.factor[i] >= 0 &&
                plan_value(soonest_clear + settle) * lanes.factor[i] - least_given_up <= best)
                continue;
            const float ours_at = std::max(float(std::max(0, lanes.path[i] - our_range)) * travel_scale, start);
            const float claim_at = std::max(lanes.claim_eta[i], start);
            // The walk out of the camps' reach from a member's attack range,
            // under the living strikers' fire (ours at our mitigation).
            const auto exit_cost = [&](int member_range) {
                return lanes.pressure[i] * std::max(0.0f, lanes.reach[i] - float(member_range - 1));
            };
            // The teammates not on it, soonest first.
            std::array<std::pair<float, unsigned>, TeamMemory::slots> soon{};
            unsigned n = 0;
            for (unsigned s = 0; s < kits; ++s) {
                const auto &m = mates[s];
                if (s == slot || !living(s) || !m.free || m.claim == int(i))
                    continue;
                const float walk = float(walk_bound(m.where, int(i)));
                const float arrival = std::max(
                    start, m.busy + (m.busy > 0 ? 0.0f : 0.5f * float(policy::replan_period)) +
                               std::max(0.0f, walk - (lanes.reach[i] + 3.0f)));
                unsigned k = n++;
                for (; k > 0 && soon[k - 1].first > arrival; --k)
                    soon[k] = soon[k - 1];
                soon[k] = {arrival, s};
            }
            const float health_left = lanes.remaining[i], work = lanes.striker_work[i];
            const float trip_time = recall_trip + lanes.home[i] * travel_scale;
            // The front (scored as in score_lanes()): the claimants', then
            // each member joining.
            float front = lanes.front_range[i], fronts = lanes.front_count[i];
            const float mates_exit = lanes.claims[i] > 0 ? exit_cost(int(front)) : 0.0f;
            float first = std::max(0.0f, lanes.front_first[i] - fronts * mates_exit),
                  visit = lanes.front_visit[i] - fronts * mates_exit;
            // The claimants' plan without us.
            float without = 0;
            if (claim_speed > 0) {
                const float trips =
                    std::ceil(std::max(0.0f, work / claim_speed - first) / std::max(100.0f, visit / std::max(1.0f, fronts)));
                without = plan_value(claim_at +
                                     std::max(health_left / claim_speed, lanes.camp_count[i] / std::max(1.0f, lanes.claims[i])) +
                                     trips * trip_time / std::max(1.0f, fronts) + settle);
            }
            const auto join = [&](int member_range, float member_first, float member_visit, float exit) {
                if (float(member_range) < front) {
                    front = float(member_range);
                    first = visit = fronts = 0;
                }
                if (float(member_range) == front) {
                    first += std::max(0.0f, member_first - exit);
                    visit += member_visit - exit;
                    fronts += 1;
                }
            };
            join(our_range, first_visit, usable, exit_cost(our_range) * camp_scale);
            const bool we_front = float(our_range) <= front;
            // With the claimants, us and the k soonest (k = 0: no party).
            float party_speed = 0, rate_asked = 0;
            for (unsigned k = 0; k <= n; ++k) {
                if (k > 0) {
                    const auto &m = mates[soon[k - 1].second];
                    party_speed += mate_rate[soon[k - 1].second] * policy::team_clear_share;
                    rate_asked += mate_rate[soon[k - 1].second] * policy::party_ask_discount;
                    // A bigger party gives up more: once this one's best
                    // possible value (the bound above) less what it asks
                    // cannot beat the best, no bigger one can.
                    if (lanes.factor[i] >= 0 &&
                        plan_value(soonest_clear + settle) * lanes.factor[i] - party_best * rate_asked / our_rate <= best)
                        break;
                    join(m.range, policy::camp_visit_health(m.health, m.max_health), m.max_health, exit_cost(m.range));
                }
                // The clear: each party strikes from its arrival, the asked
                // ones from the last of them.
                std::array<Cohort, 3> cohorts{{{ours_at, speed},
                                               {claim_at, claim_speed},
                                               {k > 0 ? soon[k - 1].first : ours_at, party_speed}}};
                // By arrival, in std::sort's order for three (libc++'s
                // __sort3, ties included) without the call.
                const auto sooner = [](const Cohort &a, const Cohort &b) { return a.at < b.at; };
                if (!sooner(cohorts[1], cohorts[0])) {
                    if (sooner(cohorts[2], cohorts[1])) {
                        std::swap(cohorts[1], cohorts[2]);
                        if (sooner(cohorts[1], cohorts[0]))
                            std::swap(cohorts[0], cohorts[1]);
                    }
                } else if (sooner(cohorts[2], cohorts[1]))
                    std::swap(cohorts[0], cohorts[2]);
                else {
                    std::swap(cohorts[0], cohorts[1]);
                    if (sooner(cohorts[2], cohorts[1]))
                        std::swap(cohorts[1], cohorts[2]);
                }
                float t = cohorts[0].at, striking = 0, left = health_left, end = -1;
                for (const auto &c : cohorts) {
                    if (striking > 0 && left <= striking * (c.at - t)) {
                        end = t + left / striking;
                        break;
                    }
                    left -= striking * (c.at - t);
                    t = c.at;
                    striking += c.speed;
                }
                if (end < 0)
                    end = t + left / striking;
                end = std::max(end, cohorts[0].at + lanes.camp_count[i] / (lanes.claims[i] + 1 + float(k)));
                const float damage = work / striking * (we_front && float(our_range) == front ? camp_scale : 1.0f) *
                                     (lanes.claims[i] > 0 || k > 0 ? 1.0f : lanes.reach_share[i]);
                const float each = visit / std::max(1.0f, fronts);
                if (each < 100)
                    continue;
                const float trips = std::ceil(std::max(0.0f, damage - first) / each);
                // Without trips home, a party adds nothing a choice cannot.
                if (k == 0) {
                    if (trips == 0)
                        break;
                    continue;
                }
                const float time = end + trips * trip_time / std::max(1.0f, fronts) + settle;
                if constexpr (policy::diagnostic)
                    if (g.unit.gem_value() >= 100 && round % 50 == 0)
                        std::printf("PARTY r=%u id=%u g=%zu k=%u end=%.0f trips=%.0f each=%.0f damage=%.0f first=%.0f time=%.0f left=%.0f "
                                    "value=%.3f without=%.3f factor=%.2f asked=%.3f best=%.3f\n",
                                    round, id(bc::self()), i, k, end, trips, each, damage, first, time, rounds_left,
                                    plan_value(time), without, lanes.factor[i], party_best * rate_asked / our_rate, best);
                if (time > rounds_left)
                    continue;
                const float score =
                    (plan_value(time) - without) * lanes.factor[i] - party_best * rate_asked / our_rate;
                if (score > best) {
                    best = score;
                    choice = int(i);
                    party = int(i);
                }
            }
        }
    }
    last_plan = round;
    // The chosen group's run, when it scored above the group alone, is taken
    // (worker_farm()): its members after the group, the next last. A choice
    // of another group ends the run being taken; the same group, a member
    // on its way, keeps it.
    if (choice < 0 || choice != target)
        cluster_run.clear();
    // A party plan's clear is the party's: no siege or run of our own.
    siege_group = choice >= 0 && party < 0 && lanes.siege_won[choice] ? choice : -1;
    request_group = party;
    if (choice >= 0 && party < 0 && cluster_run.empty() && lanes.run_won[choice]) {
        gather_run(std::size_t(choice), &cluster_run);
        std::reverse(cluster_run.begin(), cluster_run.end());
    }
    return choice;
}

void Bot::worker_farm() {
    profiling::Scope scope(profiling::Stage::farm);
    auto &me = bc::self();
    // A teammate's claim on a member of our cluster run, or an enemy seen
    // near one, ends the run: walking on would find it taken or meet them.
    for (const int member : cluster_run)
        if (teammate_claims(member) || enemy_near_lately(member)) {
            cluster_run.clear();
            break;
        }
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
            const bool bounced = policy::bounce_stalls && bot.position == bot.before_previous && bot.position != from;
            bot.bounces = bounced ? bot.bounces + 1 : 0;
            const bool idle = (bot.position == from || bot.bounces >= policy::bounce_min) && t >= 0 &&
                              distance(bot.position, bot.groups[t].centre) > bot.range + 2 && self.is_alive() &&
                              self.state().contains(ready) && !bot.groups[t].tiles().is_set(bot.position);
            bot.farm_stalls = idle ? bot.farm_stalls + 1 : 0;
        }
    } farm_stalls_count{*this, position};
    // A capture we held paid: the cluster run takes its next member, open on
    // our arrival, without a new choice; one no longer open ends the run, and
    // camp choice plans again as for any finished group.
    if (target >= 0 && groups[target].status == CampGroup::cooling && capture_group == target &&
        capture_since >= 0 && !cluster_run.empty()) {
        if (run_continues()) {
            target = cluster_run.back();
            cluster_run.pop_back();
            last_plan = round; // This turn's choice.
        } else
            cluster_run.clear();
    }
    const bool finished = target >= 0 && groups[target].status == CampGroup::cooling;
    // A capture a few steps away that we outrank its holders on (the bigger
    // bag or first there, capture_outranked()) is ours to step onto: camp choice, which
    // leaves captures to the teammates on them, does not take it from us.
    const bool taking = target >= 0 && groups[target].status == CampGroup::capture &&
                        distance(position, groups[target].centre) <= range + 2 && !capture_outranked(groups[target]);
    // A clear once under way (its camps damaged) with us at it is seen
    // through: re-planning every few rounds pulled champions off monoliths
    // mid-clear (48 of 371 re-plans in four monolith games, one at 3,829
    // health of 20,000), and off home wolf packs to captures and groups
    // elsewhere (split, quarry, gemfight: a pack left at 520 of 1,350, one
    // at 910 by a Vendetta at 1,937 health); nobody came back for the cheap
    // rest, half-cleared packs standing 31 rounds each to r600 (print dont
    // int's none). Going home hurt and fights still come first (the turn's
    // order).
    bool engaged = false;
    if (target >= 0 && groups[target].status == CampGroup::alive && groups[target].health < groups[target].max_health) {
        const auto &h = groups[target];
        engaged = distance(position, h.centre) <= h.live_reach + 3;
    }
    // At most one choice per turn: one made earlier this turn stands.
    if (last_plan != int(round) &&
        (target < 0 || finished || (!taking && !engaged && int(round) - last_plan >= policy::replan_period))) {
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
    {
        // A teammate asks for a partner at our group: we answer, coming.
        // Our own party plan asks (camp choice's request_group); either
        // claims the group.
        unsigned code = target < int(TeamMemory::bank) - 1 ? unsigned(target + 1) : 0;
        const unsigned asked = shared.camp_request(target);
        bool answering = false;
        for (unsigned s = 0; asked && s < roster.size() && s < TeamMemory::slots; ++s)
            if (s != slot && roster[s].is_alive() && shared.get(s) == asked)
                answering = true;
        if (answering)
            code = shared.camp_coming(target);
        else if (request_group == target && asked)
            code = asked;
        publish(code);
    }
    if (g.status == CampGroup::capture) {
        if constexpr (policy::diagnostic)
            std::printf("HOLD r=%u id=%u pos=%d,%d on=%d contested=%d enemies=%zu\n", round, id(bc::self()),
                        position.x, position.y, int(g.tiles().is_set(position)), g.contested, enemies.size());
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
        // Contested by an enemy out of view (observe_group()): walk the
        // tiles to the end nearest the enemy spawn and back until it is in
        // view. The fight is then skirmish's, before farming.
        else if (policy::sweep_contested && g.contested == int(round) && enemies.empty()) {
            if constexpr (policy::diagnostic)
                std::printf("SWEEP r=%u id=%u pos=%d,%d back=%d\n", round, id(bc::self()), position.x, position.y,
                            int(sweep_back));
            vec2 far = g.centre;
            int best = -1;
            for (const auto p : g.tiles().positions()) {
                const int d = distance(p, sweep_back ? home_centre : enemy_centre);
                if (best < 0 || d < best) {
                    best = d;
                    far = p;
                }
            }
            if (distance(position, far) <= 1)
                sweep_back = !sweep_back;
            else
                move_to(around(far, 1), far, false, true);
        }
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
            if (homeward)
                cluster_run.clear(); // Home ends the run.
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
    // A spell-range siege as camp choice planned it (siege_group), unless an
    // ally is closer to the camps: they attack the nearest champion, so with
    // a closer ally we attack from normal range. Farming decided it again
    // from another formula (our rate alone, its own threshold) and could
    // disagree with the plan.
    const float spell_range = siege_reach;
    bool siege = siege_group == target && !ally_draws_attacks(g);
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
    // Strikable: a living camp in sight and in our range from here. One in
    // sight out of range and one in range out of sight (in a bush) left
    // nothing to strike, yet ended the closing in: 26 rounds at 00quarry's
    // mirrored gold clusters.
    bool sighted = false, strikable = false;
    for (const auto &c : g.camps)
        if (c.health > 0 && visible.is_set(c.position)) {
            sighted = true;
            strikable = strikable || distance(position, c.position) <= range;
        }
    // Closing in ends once a camp of the group is in sight and in range from
    // here, or once we struck one last turn: a ranged champion striking from
    // outside the remembered range was otherwise sent beside the camps.
    const bool in_range = goals.is_set(position);
    if (unseen_group != target || (sighted && (strikable || g.attacked >= int(round) - 1)))
        unseen_group = -1;
    if (!strikable && in_range)
        unseen_group = target;
    if (unseen_group == target) {
        auto beside = around(nearest->position, 1);
        // A camp in a bush is seen only from inside its bush (the rules: the
        // bush you stand in is clear to your own sight): beside it from out
        // of the bush it never came into view, and ours stood there 40-150
        // rounds (00quarry's gold cluster at 28,10). Close in on its bush
        // patch within our range.
        if (bush_tile[tile_index(nearest->position)]) {
            static std::vector<vec2> frontier;
            bc::bitboard patch;
            frontier.assign(1, nearest->position);
            patch.set(nearest->position);
            const float reach = float(std::max(1, range));
            while (!frontier.empty()) {
                const vec2 p = frontier.back();
                frontier.pop_back();
                for (const vec2 step : {vec2{1, 0}, vec2{-1, 0}, vec2{0, 1}, vec2{0, -1}}) {
                    const vec2 q{p.x + step.x, p.y + step.y};
                    if (q.x < 0 || q.y < 0 || q.x >= map_width || q.y >= map_height || patch.is_set(q) ||
                        !bush_tile[tile_index(q)] || float(distance(q, nearest->position)) > reach)
                        continue;
                    patch.set(q);
                    frontier.push_back(q);
                }
            }
            subtract(patch, blocked);
            if (!patch.is_empty())
                beside = std::move(patch);
        }
        subtract(beside, blocked);
        if (!beside.is_empty()) {
            goals = std::move(beside);
            siege = false;
        }
    }
    // A ranged champion strikes from the tiles in its range of the camps
    // that the least living strength would strike: a camp strikes the
    // nearest champion in its reach, so one with an ally in view nearer to
    // it (strictly: the rules name no tie-break) strikes the ally. Print
    // dont int's Vesper stands 3 from the camps behind its tank at 1, struck
    // on 10% of rounds when a teammate is nearer (89% when it is nearest);
    // alone, the tiles the fewest camps reach (CampGroup::reach_share, as
    // camp choice plans it).
    if (range > 1 && !siege && unseen_group != target) {
        // Each camp that strikes (clusters strike for nothing) reaches the
        // tiles within its attack range that no living ally is nearer it
        // than: within the lesser of the two. It adds its strength to them,
        // and each tile in our range reads its sum. Tile by tile against
        // every camp and ally, this cost 94k fuel a turn on big maps and
        // 244k on the-pack's 48-wolf group.
        static std::vector<vec2> tiles;
        static std::vector<std::uint32_t> hits;
        const std::size_t map_tiles = std::size_t(map_width) * std::size_t(map_height);
        tiles.resize(map_tiles);
        hits.resize(map_tiles);
        const std::uint32_t count = goals.positions_into(tiles);
        // Only these are read: the sums elsewhere are left as they come.
        for (std::uint32_t t = 0; t < count; ++t)
            hits[std::size_t(tiles[t].y * map_width + tiles[t].x)] = 0;
        for (const auto &c : g.camps) {
            if (c.health <= 0 || c.stats.base_strength <= 0)
                continue;
            int reach = int(c.stats.base_attack_range);
            for (const auto &a : allies)
                if (a.health > 0)
                    reach = std::min(reach, distance(a.position, c.position));
            const auto strength = std::uint32_t(c.stats.base_strength);
            const int x0 = std::max(0, c.position.x - reach), x1 = std::min(map_width - 1, c.position.x + reach);
            for (int y = std::max(0, c.position.y - reach); y <= std::min(map_height - 1, c.position.y + reach); ++y) {
                std::uint32_t *row = hits.data() + std::size_t(y) * std::size_t(map_width);
                for (int x = x0; x <= x1; ++x)
                    row[x] += strength;
            }
        }
        // The least sum, then the tiles with it as bits imported in one engine
        // call: most tiles tie (beyond every camp's reach), and a push and a
        // set per tile cost about 30k a turn on the-pack.
        std::uint32_t least = std::numeric_limits<std::uint32_t>::max();
        for (std::uint32_t t = 0; t < count; ++t)
            least = std::min(least, hits[std::size_t(tiles[t].y * map_width + tiles[t].x)]);
        std::uint32_t ties = 0;
        for (std::uint32_t t = 0; t < count; ++t)
            ties += hits[std::size_t(tiles[t].y * map_width + tiles[t].x)] == least;
        // Every tile tied: the goals stand as they are.
        if (ties && ties != count) {
            static std::vector<std::uint8_t> best_bits;
            best_bits.assign(bc::bitboard::linear_buffer_size(), 0);
            for (std::uint32_t t = 0; t < count; ++t) {
                const std::size_t index = std::size_t(tiles[t].y * map_width + tiles[t].x);
                if (hits[index] == least)
                    best_bits[index >> 3] |= std::uint8_t(1u << (index & 7));
            }
            goals.import_linear_buffer(best_bits);
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
            note_group_due(std::size_t(target));
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
            return mate_claim(s);
    return -1;
}

namespace {
// What a capture pays a champion of this type, in gold: the group's gold
// with Vesper's bonus, and its gems at Marcella's gold per gem tributed
// (the game's passives; items left out, as teammates' items are not seen,
// so every champion ranks alike).
float capture_worth(bc::champion_type type, const CampGroup &g) {
    namespace passives = bc::constants::passive_abilities;
    float worth = float(g.unit.gold_value());
    if (type == bc::champion_type::vesper_vale)
        worth *= 1 + float(passives::vesper_vale_bonus_gold_percent) / 100;
    if (type == bc::champion_type::marcella)
        worth += float(g.unit.gem_value()) * float(passives::marcella_tribute_gold_per_gem) +
                 policy::marcella_capture_bonus;
    return worth;
}
} // namespace

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
        if (ally.channel || outranks(ally, !on, g))
            return true;
    }
    return false;
}

bool Bot::capture_deferred(const CampGroup &g) const {
    // A teammate claiming the group that rides its capture's recall home or
    // outranks us (outranks(): on the tiles, or within 3 by the capture's
    // worth, then the bigger bag) holds it from the start:
    // two on the tiles split the reward (a fifth of our captures, willow
    // best girl's 2%).
    const int group = int(&g - groups.data());
    for (const auto &ally : allies)
        if (ally.health > 0 && claimed_by(ally) == group && distance(ally.position, g.centre) <= 3 &&
            (ally.channel ||
             (g.tiles().is_set(ally.position)
                  ? outranks(ally, true, g)
                  : capture_worth(ally.type, g) > capture_worth(type, g) ||
                        (capture_worth(ally.type, g) == capture_worth(type, g) && ally.gems > int(gems)))))
            return true;
    return false;
}

int Bot::capture_payout(const CampGroup &g) const {
    // The timer starts at the first capture phase held, no earlier than the
    // round after we stepped on, and pays capture_time phases later
    // (docs/GameRules.md: a five-round timer counted down by later phases).
    // Our own stepping on counts only for the group we hold (capture_group).
    const int stepped_on = capture_group == int(&g - groups.data()) ? capture_since : -1;
    return std::max(g.holding_since, stepped_on + 1) + rules::settings().capture_time;
}

bool Bot::outranks(const ObservedUnit &ally, bool arriving, const CampGroup &g) const {
    // The holder order of capture_outranked() and capture_deferred(): the
    // one the capture pays most (capture_worth()), then the bigger bag, then
    // (on the tiles) one already there, then the lower id. Print dont int's
    // Vesper or Marcella hold 82% of their captures of groups worth 90+
    // gold (ours 40%, our Ryth 31%), their Vesper earning 38-41% of the
    // team's gold to our 22% and its items running ahead from round 100.
    const float theirs = capture_worth(ally.type, g), ours = capture_worth(type, g);
    if (theirs != ours)
        return theirs > ours;
    if (ally.gems != int(gems))
        return ally.gems > int(gems);
    return arriving || id(ally.champion()) < id(bc::self());
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
    // Hurt: home (a little less eagerly when behind in the gem race).
    if (health < max_health * policy::home_capture_health * (1.0f - 0.5f * behind_pressure()))
        return true;
    // An emblem's gold: home to buy it, power for the rest of the game.
    if (bc::self().gold_count() >= policy::home_capture_gold)
        return true;
    // A cluster run (worker_choose_group()) with its next member open goes
    // on: the run was planned to end in this recall after its last capture.
    // Returning false makes the next turn cancel this recall; each capture
    // asks again.
    if (run_continues())
        return false;
    // A short trip out ends home.
    return int(round) - left_spawn < policy::short_trip_rounds;
}

void Bot::link_clusters() {
    // Once: which groups are cluster groups (gem or gold clusters only:
    // nothing strikes back, one or two hits each), none linked yet.
    if (cluster_group.size() == groups.size())
        return;
    cluster_group.assign(groups.size(), 0);
    link_first.assign(groups.size(), -1);
    link_count.assign(groups.size(), 0);
    link_checked.assign(groups.size(), 0);
    for (std::size_t i = 0; i < groups.size(); ++i) {
        bool cluster = !groups[i].camps.empty();
        for (const auto &c : groups[i].camps)
            cluster = cluster && c.stats.base_strength <= 0;
        cluster_group[i] = cluster;
    }
}

std::span<const Bot::ClusterLink> Bot::cluster_links_of(int g) {
    // The cluster groups whose nearest camp tiles a walk joins in no more
    // rounds than a capture takes. Walks are never shorter than Chebyshev,
    // so the groups within capture_time Chebyshev are every candidate: found
    // on first need (the map fixes them), four camps a step, at their
    // Chebyshev gap. Each gap becomes the walk once either group's walk
    // field is built, checked as fields come. Nearest first.
    bool order = false;
    if (link_first[g] < 0) {
        // Only camps of other cluster groups within the group's bounding
        // box widened by the reach can be in reach of one of its camps:
        // those are found in one pass, four camps a step, and each is
        // measured against the group's camps. Each linked group is listed
        // in the order of its first camp pair in reach (by the group's camp,
        // then camp order), at the nearest such pair's gap.
        const int reach = rules::settings().capture_time;
        const auto &own = groups[g].camps;
        int low_x = std::numeric_limits<int>::max(), high_x = std::numeric_limits<int>::min();
        int low_y = low_x, high_y = high_x;
        for (const auto &c : own) {
            low_x = std::min(low_x, c.position.x);
            high_x = std::max(high_x, c.position.x);
            low_y = std::min(low_y, c.position.y);
            high_y = std::max(high_y, c.position.y);
        }
        struct Found {
            int group;
            std::size_t first_camp, first_tile;
            int apart;
        };
        static std::vector<Found> found;
        static std::vector<std::uint32_t> entry_stamp, entry;
        static std::uint32_t stamp = 0;
        entry_stamp.resize(groups.size());
        entry.resize(groups.size());
        ++stamp;
        found.clear();
        const std::size_t own_lane = std::size_t(own.data() - camp_pool.data());
        const v128_t wide_reach = wasm_i32x4_splat(reach);
        if (!own.empty()) {
            const v128_t lx = wasm_i32x4_splat(low_x - reach), hx = wasm_i32x4_splat(high_x + reach);
            const v128_t ly = wasm_i32x4_splat(low_y - reach), hy = wasm_i32x4_splat(high_y + reach);
            for (std::size_t k = 0; k < camp_lanes; k += 4) {
                const v128_t x = wasm_v128_load(&camp_x[k]), y = wasm_v128_load(&camp_y[k]);
                const v128_t in = wasm_v128_and(wasm_v128_and(wasm_i32x4_ge(x, lx), wasm_i32x4_le(x, hx)),
                                                wasm_v128_and(wasm_i32x4_ge(y, ly), wasm_i32x4_le(y, hy)));
                for (unsigned bits = wasm_i32x4_bitmask(in); bits; bits &= bits - 1) {
                    const std::size_t j = k + unsigned(__builtin_ctz(bits));
                    const int other = camp_group[j];
                    if (other == g || !cluster_group[other])
                        continue;
                    // The group's camps are lanes own_lane on, in its camp
                    // order: four at a time, the rest one by one.
                    const v128_t tx = wasm_i32x4_splat(camp_x[j]), ty = wasm_i32x4_splat(camp_y[j]);
                    std::size_t first_camp = own.size();
                    v128_t low = wasm_i32x4_splat(std::numeric_limits<std::int32_t>::max());
                    std::size_t ci = 0;
                    for (; ci + 4 <= own.size(); ci += 4) {
                        const v128_t d =
                            wasm_i32x4_max(wasm_i32x4_abs(wasm_i32x4_sub(tx, wasm_v128_load(&camp_x[own_lane + ci]))),
                                           wasm_i32x4_abs(wasm_i32x4_sub(ty, wasm_v128_load(&camp_y[own_lane + ci]))));
                        const v128_t in = wasm_i32x4_le(d, wide_reach);
                        if (!wasm_v128_any_true(in))
                            continue;
                        if (first_camp == own.size())
                            first_camp = ci + unsigned(__builtin_ctz(unsigned(wasm_i32x4_bitmask(in))));
                        low = wasm_i32x4_min(low, d);
                    }
                    low = wasm_i32x4_min(low, wasm_i32x4_shuffle(low, low, 2, 3, 0, 1));
                    low = wasm_i32x4_min(low, wasm_i32x4_shuffle(low, low, 1, 0, 3, 2));
                    int apart = wasm_i32x4_extract_lane(low, 0);
                    const vec2 tile{camp_x[j], camp_y[j]};
                    for (; ci < own.size(); ++ci)
                        if (const int d = distance(own[ci].position, tile); d <= reach) {
                            first_camp = std::min(first_camp, ci);
                            apart = std::min(apart, d);
                        }
                    if (first_camp == own.size())
                        continue;
                    if (entry_stamp[other] != stamp) {
                        entry_stamp[other] = stamp;
                        entry[other] = std::uint32_t(found.size());
                        found.push_back({other, first_camp, j, apart});
                    } else {
                        auto &f = found[entry[other]];
                        if (first_camp < f.first_camp) {
                            f.first_camp = first_camp;
                            f.first_tile = j;
                        }
                        f.apart = std::min(f.apart, apart);
                    }
                }
            }
        }
        // Listed by first pair (camp order is the tiles' order within one).
        for (std::size_t e = 1; e < found.size(); ++e)
            for (std::size_t f = e; f > 0 && std::tie(found[f].first_camp, found[f].first_tile) <
                                                 std::tie(found[f - 1].first_camp, found[f - 1].first_tile);
                 --f)
                std::swap(found[f], found[f - 1]);
        const std::size_t first = cluster_links.size();
        for (const auto &f : found)
            cluster_links.push_back({f.group, f.apart, false});
        link_first[g] = std::int32_t(first);
        link_count[g] = std::uint16_t(cluster_links.size() - first);
        link_checked[g] = walk_cursor + 1; // Its walks are checked below.
        order = true;
    }
    ClusterLink *links = cluster_links.data() + link_first[g];
    const std::size_t count = link_count[g];
    if (link_checked[g] != walk_cursor) {
        link_checked[g] = walk_cursor;
        for (std::size_t e = 0; e < count; ++e)
            if (!links[e].walked && (walk_of[g] || walk_of[links[e].group])) {
                links[e].apart = cluster_walk(g, links[e].group);
                links[e].walked = order = true;
            }
    }
    if (order)
        for (std::size_t e = 1; e < count; ++e)
            for (std::size_t f = e; f > 0 && links[f].apart < links[f - 1].apart; --f)
                std::swap(links[f], links[f - 1]);
    return {links, count};
}

int Bot::cluster_walk(int a, int b) const {
    // A walk field holds the walk from each tile to its group's nearest camp,
    // so the least over the other group's camps is the walk between them.
    const bool from_b = walk_of[b] != nullptr;
    const std::uint16_t *walks = from_b ? walk_of[b] : walk_of[a];
    int walk = std::numeric_limits<int>::max();
    for (const auto &c : groups[from_b ? a : b].camps)
        walk = std::min(walk, int(walks[c.tile]));
    return walk;
}

bool Bot::teammate_claims(int group) const {
    for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
        if (mate_claim(s) == group)
            return true;
    return false;
}

bool Bot::enemy_near_lately(int group) const {
    // Within policy::worker_sighting_memory rounds (worker_track_sightings()).
    return std::size_t(group) < enemy_seen_near.size() &&
           int(round) - enemy_seen_near[group] <= policy::worker_sighting_memory;
}

bool Bot::cluster_open(int group, int arrival, bool claimed) const {
    const auto &n = groups[group];
    return !claimed && !enemy_near_lately(group) &&
           (n.status == CampGroup::alive || (n.status == CampGroup::cooling && n.respawn <= arrival));
}

bool Bot::run_continues() const {
    if (cluster_run.empty())
        return false;
    const int next = cluster_run.back();
    int walk = std::numeric_limits<int>::max();
    if (const std::uint16_t *walks = walk_of[next])
        walk = walks[tile_index(position)];
    else
        for (const auto &c : groups[next].camps)
            walk = std::min(walk, distance(position, c.position));
    // A champion with a dash walks the faster (camp choice's travel scale).
    float scale = 1;
    for (const auto &s : spells)
        if (s.kind == bc::ability_kind::dash)
            scale = policy::dash_travel_scale;
    return cluster_open(next, int(round) + int(float(walk) * scale), teammate_claims(next));
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
    // The gem race: each side's tributed gems and the bags we know of (ours,
    // teammates' in view; enemies' as last
    // seen, alive), and the gap against what the leader still needs to win.
    // Early on the leader needs most of the race, so a gap weighs little; a
    // gap the leader is about to close weighs fully. (A gap against 80 gems
    // from round 150 on read 40 tributed gems behind at round 200 as fully
    // behind.)
    if (behind_round == int(round))
        return behind_value;
    behind_round = int(round);
    float ours = float(bc::global_submitted_gems(bc::team::ally)) + float(gems);
    for (const auto &a : allies)
        ours += float(a.gems);
    float theirs = float(bc::global_submitted_gems(bc::team::enemy));
    for (std::size_t i = 0; i < ghost_seen.size(); ++i)
        if (ghost_seen[i] && !(i < enemy_respawn.size() && enemy_respawn[i] >= 0))
            theirs += ghost_seen[i]->tributing ? 0.0f : float(ghost_seen[i]->gems);
    const float win = float(rules::settings().winning_gems);
    behind_gap = theirs - ours;
    behind_value = std::clamp((theirs - ours) / std::max(1.0f, win - theirs), 0.0f, 1.0f);
    return behind_value;
}

