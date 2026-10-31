// Worker camp choice and farming.
#include "../bot.h"
#include <tuple>
#include <wasm_simd128.h>

int Bot::worker_choose_group(int excluded) {
    // A group we were walled off from lately is no candidate.
    const int walled = int(round) < stuck_group_until ? stuck_group : -1;
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
    // Exactly one living non-Prism may take an opportunistic enemy-side gold
    // detour while teammates reserve a cooling camp. A stable roster order
    // prevents the whole team from making the same locally attractive trip.
    bool cooling_detour_runner = type != bc::champion_type::prism;
    for (unsigned s = 0; cooling_detour_runner && s < slot; ++s)
        if (living(s) && opening.roster_types[s] != bc::champion_type::prism)
            cooling_detour_runner = false;
    const bool depleted = float(alive_allies) <= float(roster.size()) * policy::depleted_alive_fraction;
    // Per group, from one pass over the claims and the units in view:
    // teammates' claims (each claimant can supply the next clear's damage, so
    // each claim is also a partner), and an ally or an enemy standing on it.
    enum : std::uint8_t { ally_on = 2, enemy_on = 4 };
    static std::vector<std::uint8_t> claims, marks;
    // A hard group's partner's arrival, in rounds (-1: its party is its
    // claimants), for its wait beyond our walk (gather()).
    static std::vector<float> partner_arrival;
    claims.assign(groups.size(), 0);
    marks.assign(groups.size(), 0);
    static std::vector<int> claimed_groups;
    claimed_groups.clear();
    // The latest cooling camp already reserved by a teammate. Its remaining
    // cooldown is an opportunity window for a short live-camp detour.
    int cooling_reservation = -1;
    for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s)
        if (const int c = mate_claim(s); c >= 0) {
            ++claims[c];
            claimed_groups.push_back(c);
            if (groups[std::size_t(c)].status == CampGroup::cooling &&
                (cooling_reservation < 0 ||
                 groups[std::size_t(c)].respawn > groups[std::size_t(cooling_reservation)].respawn))
                cooling_reservation = c;
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
    // And the healing each clear gets (group_heal(), in one pass).
    static std::vector<float> claim_heals;
    claim_heals.assign(groups.size(), self_heal());
    for (unsigned s = 0; s < kits; ++s)
        if (const int c = mate_claim(s); c >= 0) {
            claim_rates[std::size_t(c)] += mate_rate[s];
            claim_heals[std::size_t(c)] += slot_heal(s);
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
            if (opening.roster_types[s] == bc::champion_type::prism && mate_claim(s) >= 0)
                prism_group = mate_claim(s);
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
        // altar term, the camp count and the walk from home.
        std::int32_t *gem, *enemy_side, *safe_gems, *hard;
        float *gem_reward, *gold, *altar, *camp_count, *home, *reach;
        // This choice's state, with the group's shared facts (hard, its
        // camps' longest reach, the strikers' work), the claimants' summed
        // camp rates, the healing a clear gets and, for a hard group nobody
        // claims, the wait for a partner.
        std::int32_t *status, *respawn, *path, *siege_open, *marks, *sighted;
        float *remaining, *splash, *pressure, *striker_work, *claims, *claim_rate, *heal, *scores, *partner_wait,
            *cohesion;
        // A hard group's party (its claimants, else the nearest free
        // teammate): its camp rate besides ours; the camp's strikes its stuns
        // take away, per round (ours too); its shields now and per round
        // (ours too); and the camp damage share of its toughest member, the
        // tank the camps strike (ours, or one in view with less).
        float *party_rate, *party_stun, *party_shield, *party_shield_rate, *tank_scale;
        // And its health (ours and its members': live in view, else their
        // types' base), what it can spend above the reserve, and its heals
        // per round (a Willow in it heals the tank).
        float *party_health, *party_spare, *party_heal;
        // A cluster group's run (gather_run): the reward and the time its
        // members add, and whether the run scored above the group alone;
        // and whether the clear is planned as a spell-range siege.
        float *run_reward, *run_time;
        std::int32_t *run_won, *siege_won;
    };
    static Lanes lanes;
    if (lanes.padded != padded) {
        lanes.padded = padded;
        lanes.ints.reset(new std::int32_t[12 * padded]);
        lanes.floats.reset(new float[26 * padded]);
        std::int32_t *next_int = lanes.ints.get();
        for (auto **lane :
             {&lanes.gem, &lanes.enemy_side, &lanes.safe_gems, &lanes.hard, &lanes.status, &lanes.respawn,
              &lanes.path, &lanes.siege_open, &lanes.marks, &lanes.sighted, &lanes.run_won, &lanes.siege_won}) {
            *lane = next_int;
            next_int += padded;
        }
        float *next_float = lanes.floats.get();
        for (auto **lane : {&lanes.gem_reward, &lanes.gold, &lanes.altar, &lanes.remaining, &lanes.splash,
                            &lanes.pressure, &lanes.claims, &lanes.scores, &lanes.striker_work,
                            &lanes.claim_rate, &lanes.heal, &lanes.camp_count, &lanes.home, &lanes.reach,
                            &lanes.partner_wait, &lanes.party_rate, &lanes.party_stun, &lanes.party_shield,
                            &lanes.party_shield_rate, &lanes.tank_scale, &lanes.party_health, &lanes.party_spare,
                            &lanes.party_heal, &lanes.run_reward, &lanes.run_time, &lanes.cohesion}) {
            *lane = next_float;
            next_float += padded;
        }
        for (std::size_t i = count; i < padded; ++i) {
            for (auto *lane : {lanes.gem, lanes.enemy_side, lanes.safe_gems, lanes.hard, lanes.status,
                               lanes.respawn, lanes.siege_open, lanes.marks})
                lane[i] = 0;
            for (auto *lane : {lanes.gem_reward, lanes.gold, lanes.altar, lanes.remaining, lanes.splash,
                               lanes.pressure, lanes.claims, lanes.striker_work, lanes.claim_rate, lanes.heal,
                               lanes.camp_count, lanes.home, lanes.reach, lanes.partner_wait, lanes.party_rate,
                               lanes.party_stun, lanes.party_shield, lanes.party_shield_rate, lanes.tank_scale,
                               lanes.party_health, lanes.party_spare, lanes.party_heal, lanes.run_reward,
                               lanes.run_time, lanes.cohesion})
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
    // Strikes a kit's stuns take from a camp per round: a stunned camp loses
    // its next strike (Prism's stun, Marcella's enhanced hit, Heart of Ruin's
    // dash landing: two of every eight strikes in the golem and monolith
    // clears watched), and our own shields.
    const auto stun_rate = [](const std::array<Spell, 2> &kit) {
        float r = 0;
        for (const auto &s : kit)
            if ((s.kind == bc::ability_kind::attack_reset && s.enhanced) ||
                ((s.kind == bc::ability_kind::cc || s.kind == bc::ability_kind::aoe_damage ||
                  (s.kind == bc::ability_kind::dash && s.modifier > 0)) &&
                 s.duration > 0))
                r += 1.0f / float(s.cooldown);
        return r;
    };
    const float own_stun = stun_rate(spells);
    float own_shield = 0, own_shield_rate = 0, own_heal = 0;
    for (const auto &s : spells) {
        if (s.kind == bc::ability_kind::shield) {
            own_shield += focus * s.modifier;
            own_shield_rate += focus * s.modifier / float(s.cooldown);
        }
        if (s.kind == bc::ability_kind::heal)
            own_heal += focus * s.modifier / float(s.cooldown);
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
            // The group's shared facts (CampGroup::refresh_live()): of its
            // living camps while it lives, else of the whole group (a capture
            // has no living camps, and was read as no hard group, reach 0).
            const bool whole = g.status != CampGroup::alive;
            remaining[i] = whole ? g.max_health : g.health;
            lanes.striker_work[i] = whole ? g.whole_work : g.live_work;
            lanes.hard[i] = (whole ? g.hard : g.live_hard) ? -1 : 0;
            lanes.reach[i] = float(whole ? g.reach : g.live_reach);
            splash[i] = g.splash;
            pressure[i] = g.pressure;
            claim[i] = float(claims[i]);
            float nearest_claim = std::numeric_limits<float>::infinity();
            for (const int claimed : claimed_groups)
                nearest_claim = std::min(nearest_claim, float(distance(g.centre, groups[claimed].centre)));
            lanes.cohesion[i] = std::isfinite(nearest_claim)
                                    ? std::max(0.0f, nearest_claim - opening.spacing)
                                    : 0.0f;
        }
        for (std::size_t i = 0; i < count; ++i)
            lanes.sighted[i] = i < enemy_seen_near.size() &&
                                       int(round) - enemy_seen_near[i] <= policy::worker_sighting_memory
                                   ? -1
                                   : 0;
        for (std::size_t i = 0; i < count; ++i) {
            lanes.claim_rate[i] = claim_rates[i];
            lanes.heal[i] = claim_heals[i];
        }
        // A hard group's party: its claimants, else the teammate who could be
        // there soonest: what keeps it busy (the rest of a clear or capture
        // it is on, a recall), then its walk (walk_bound(): from where it is
        // in view, else from its claimed group, else from home). One in a
        // fight, or on its way to one, is not free. Its arrival beyond our
        // walk is the wait, measured again whenever our walk is. The nearest
        // by straight line, busy or not, was waited for by champions that
        // stood beside golems for a partner that never came.
        partner_arrival.assign(count, -1.0f);
        for (std::size_t i = 0; i < count; ++i) {
            lanes.partner_wait[i] = 0;
            if (!lanes.hard[i])
                continue;
            unsigned party = 0;
            int nearest = -1;
            float soonest = std::numeric_limits<float>::max();
            for (unsigned s = 0; s < roster.size() && s < TeamMemory::slots; ++s) {
                if (s == slot || !living(s))
                    continue;
                const unsigned code = shared.get(s);
                if (code == i + 1) {
                    party |= 1u << s;
                    continue;
                }
                if (code == TeamMemory::unavailable || TeamMemory::fight_group(code, groups.size()) >= 0)
                    continue;
                vec2 where = home_centre;
                float busy = 0;
                if (code >= 1 && code <= groups.size()) {
                    const auto &h = groups[code - 1];
                    where = h.centre;
                    if (h.status == CampGroup::capture) // Its holding's timer; unknown counts from now.
                        busy = float(std::max(0, (h.holding_since >= 0 ? h.holding_since : int(round)) +
                                                     capture_time - int(round)));
                    else if (h.status == CampGroup::alive && h.health < h.max_health)
                        busy = h.health / std::max(1.0f, mate_rate[s]) + float(capture_time + 1);
                } else if (code == TeamMemory::recall)
                    busy = float(recall_turns);
                if (const int k = slot_ally[s]; k >= 0)
                    where = allies[std::size_t(k)].position;
                if (const float arrival = busy + float(walk_bound(where, int(i))); arrival < soonest) {
                    soonest = arrival;
                    nearest = int(s);
                }
            }
            if (!party && nearest >= 0) {
                party = 1u << nearest;
                partner_arrival[i] = soonest;
                lanes.partner_wait[i] = std::max(0.0f, soonest - float(lanes.path[i]));
            } else if (!party && groups[i].attacked < int(round) - rules::settings().camp_sleep_time - 1)
                // Nobody free to come, and unstruck: worker_farm() would wait to the end.
                lanes.partner_wait[i] = float(int(rules::settings().max_rounds) - int(round));
            float rate = 0, stun = own_stun, shield = own_shield, shield_rate = own_shield_rate,
                  tank = camp_mitigation * mitigation, party_health = health, party_spare = spare, heal = own_heal;
            for (unsigned s = 0; s < kits; ++s) {
                if (!(party >> s & 1))
                    continue;
                float mate_focus = roster_kits[s].focus, mate_health = roster_kits[s].health,
                      mate_max = roster_kits[s].health;
                const bool in_view = slot_ally[s] >= 0;
                if (in_view) {
                    const auto &ally = allies[std::size_t(slot_ally[s])];
                    mate_focus = ally.focus;
                    mate_health = ally.health;
                    mate_max = ally.max_health;
                    tank = std::min(tank, ally.camp_mitigation * ally.mitigation);
                }
                // Out of view, its Jungle emblem from its team memory flag.
                if (!in_view && shared.jungle(s))
                    tank = std::min(tank, jungle_share);
                party_health += mate_health;
                party_spare += std::max(0.0f, mate_health - mate_max * policy::casualty_margin);
                rate += mate_rate[s];
                stun += stun_rate(roster_kits[s].spells);
                for (const auto &spell : roster_kits[s].spells) {
                    if (spell.kind == bc::ability_kind::shield) {
                        shield += mate_focus * spell.modifier;
                        shield_rate += mate_focus * spell.modifier / float(spell.cooldown);
                    }
                    if (spell.kind == bc::ability_kind::heal)
                        heal += mate_focus * spell.modifier / float(spell.cooldown);
                }
            }
            lanes.party_health[i] = party_health;
            lanes.party_spare[i] = party_spare;
            lanes.party_heal[i] = heal;
            lanes.party_rate[i] = rate;
            lanes.party_stun[i] = std::min(1.0f, stun);
            lanes.party_shield[i] = shield;
            lanes.party_shield_rate[i] = shield_rate;
            lanes.tank_scale[i] = tank;
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
    const float gold_weight =
        gold_value * gold_bonus * (type == bc::champion_type::willow ? policy::willow_gold_factor : 1.0f);
    // Remaining time in the team's latest cooling reservation. A detour below
    // is eligible only when its own full duration fits in this idle window.
    const float cooling_window =
        cooling_reservation >= 0
            ? float(std::max(0, groups[std::size_t(cooling_reservation)].respawn - int(round)))
            : 0.0f;
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
    const float our_focus = focus, our_multiplier = camp_multiplier, camp_scale = camp_mitigation * mitigation;
    const std::array<Spell, 2> kit = spells;
    // Cluster runs. A short trip's capture ends in a recall home
    // (home_after_capture()), which every version that kept farming instead
    // lost 5-9 games of 80 to; but a row of gem or gold clusters a tile or
    // two apart (each its own group and capture) was then taken one per
    // trip. A cluster group's run is the clusters taken after it on the same
    // trip: next, the nearest cluster group linked to the last member
    // (cluster_links_of()) that is not in the run and is open on arrival
    // (cluster_open()), while the capture that pays it still ends in the
    // short trip's recall: the recall starts recall_turns before the
    // payout, within short_trip_rounds of leaving spawn. Only a champion
    // that recalls after a short trip, during one, plans runs (not the
    // Prism, which keeps its gems).
    const bool runs = passive != bc::passive_type::gem_focus && now - left_spawn < policy::short_trip_rounds;
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
            // Hard groups (golems, monoliths) are cleared as a party
            // (policy::hard_camp_health): no crowding, and one nobody claims
            // counts the partner it waits for, its damage as ours.
            const v128_t hard = wasm_v128_load(&lanes.hard[i]);
            crowding = pick(zero, crowding, hard);
            const v128_t unpartnered = wasm_v128_and(hard, wasm_f32x4_eq(claimants, zero));
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
            const v128_t helpers = pick(one, claimants, unpartnered);
            // The team's summed camp rates: ours and the claimants' (a hard
            // group's coming partner as ourselves).
            const v128_t own_rate = wasm_f32x4_mul(rate, splat(our_multiplier));
            const v128_t team_rate = wasm_f32x4_add(
                own_rate, pick(wasm_v128_load(&lanes.party_rate[i]), wasm_v128_load(&lanes.claim_rate[i]), unpartnered));
            const v128_t speed =
                wasm_f32x4_pmax(one, wasm_f32x4_mul(team_rate, splat(policy::team_clear_share)));
            v128_t clear = wasm_f32x4_div(remaining, speed);
            // Each camp takes at least one hit.
            clear = wasm_f32x4_pmax(clear, wasm_f32x4_div(camps_in_group, wasm_f32x4_add(one, helpers)));
            // The clear's camp damage: the strikers' work at the clearing speed,
            // in rounds of a striker's strength.
            const v128_t striker_rounds = wasm_f32x4_div(wasm_v128_load(&lanes.striker_work[i]), speed);
            v128_t loss = wasm_f32x4_mul(striker_rounds, splat(camp_scale));
            // A hard group's party clear: the camps strike the tank, less
            // what the party's stuns take away, and its shields soak some.
            // Struck every round at our own share, a monolith clear came to
            // tens of thousands; the clears watched cost 3,500-4,000.
            {
                const v128_t strikes = wasm_f32x4_mul(
                    striker_rounds, wasm_f32x4_pmax(zero, wasm_f32x4_sub(one, wasm_v128_load(&lanes.party_stun[i]))));
                v128_t party_loss = wasm_f32x4_mul(strikes, wasm_v128_load(&lanes.tank_scale[i]));
                party_loss = wasm_f32x4_sub(
                    party_loss, wasm_f32x4_add(wasm_v128_load(&lanes.party_shield[i]),
                                               wasm_f32x4_mul(wasm_v128_load(&lanes.party_shield_rate[i]), clear)));
                loss = pick(wasm_f32x4_pmax(zero, party_loss), loss, hard);
            }
            // The clear's camp damage before splitting it between tank and
            // helpers (for multi-visit clears).
            const v128_t team_loss = loss;
            // With a helper the camps strike the tank: a ranged champion takes
            // few of the hits, a melee one more.
            loss = pick(wasm_f32x4_mul(loss, splat(our_range > 1 ? policy::ranged_helped_loss
                                                             : policy::melee_helped_loss)),
                        loss, wasm_f32x4_ge(helpers, one));
            // Our own shields and stuns (a hard group's party counted them).
            v128_t saved = zero;
            for (const auto &spell : kit) {
                const v128_t cooldown = splat(float(spell.cooldown));
                if (spell.kind == bc::ability_kind::shield)
                    saved = wasm_f32x4_add(
                        saved, wasm_f32x4_mul(splat(our_focus * spell.modifier),
                                              wasm_f32x4_add(one, wasm_f32x4_div(clear, cooldown))));
                if (spell.kind == bc::ability_kind::cc)
                    saved = wasm_f32x4_add(saved, wasm_f32x4_div(striker_rounds, cooldown)); // While strikers live.
            }
            loss = wasm_f32x4_sub(loss, pick(zero, saved, hard));
            loss = wasm_f32x4_sub(loss, wasm_f32x4_mul(wasm_v128_load(&lanes.heal[i]), clear));
            loss = wasm_f32x4_pmax(zero, loss);
            v128_t risk = wasm_f32x4_div(wasm_f32x4_pmax(zero, wasm_f32x4_sub(loss, splat(spare))),
                                         splat(health_scale));
            // A hard group's clear is the party's: its camp damage less the
            // party's heals over the clear and what its members can spend,
            // against the party's health (the camps strike the tank, which a
            // fresh member relieves; a Willow heals it). Split by fixed
            // shares against our own health it read as a clear we must
            // survive alone.
            risk = pick(wasm_f32x4_div(
                            wasm_f32x4_pmax(zero, wasm_f32x4_sub(wasm_f32x4_sub(team_loss, wasm_f32x4_mul(wasm_v128_load(&lanes.party_heal[i]), clear)),
                                                                 wasm_v128_load(&lanes.party_spare[i]))),
                            wasm_f32x4_pmax(splat(100.0f), wasm_v128_load(&lanes.party_health[i]))),
                        risk, hard);
            v128_t ranged_dps = zero;
            for (const auto &spell : kit) {
                const bool area = spell.kind == bc::ability_kind::aoe_damage;
                // Area spells, and the targeted spells that can target a camp.
                if (siege_spell(spell))
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
            wasm_v128_store(&lanes.siege_won[i], sieging);
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
            const v128_t waiting = wasm_f32x4_add(
                wasm_f32x4_mul(
                    wasm_v128_and(is_cooling, wasm_f32x4_pmax(zero, wasm_f32x4_sub(respawn_in, travel))),
                    splat(policy::respawn_wait_weight)),
                wasm_v128_load(&lanes.partner_wait[i]));
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
            // A hard group's party shares that walk as it shares the clear:
            // the camps strike the nearest, so the party's walk out costs one
            // walk between them. Counted whole, it ruled monoliths out in
            // 43-95% of the choices on monolith maps (Willow's most).
            const v128_t exit_steps =
                wasm_f32x4_pmax(zero, wasm_f32x4_sub(wasm_v128_load(&lanes.reach[i]), splat(float(our_range) - 1)));
            v128_t exit_cost = wasm_f32x4_mul(wasm_f32x4_mul(pressure, exit_steps), splat(camp_scale));
            exit_cost = pick(wasm_f32x4_div(exit_cost, wasm_f32x4_add(one, helpers)), exit_cost, hard);
            const v128_t visit_health = wasm_f32x4_sub(splat(usable), exit_cost);
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
            // A hard group is cleared by its party in one go: the tank swaps out
            // and a Willow heals it, nobody goes home mid-clear (golem and
            // monolith clears watched), so the trips-home plan is not its plan.
            value = wasm_f32x4_pmax(value, pick(zero, visits, hard));
            // A cluster group's run (gather_run) is one plan: the group's
            // reward and time with its members', under the same hazards. The
            // better of the two stands; with no run it is the group's own.
            {
                const v128_t run_duration = wasm_f32x4_add(duration, wasm_v128_load(&lanes.run_time[i]));
                v128_t run = wasm_f32x4_div(wasm_f32x4_add(reward, wasm_v128_load(&lanes.run_reward[i])),
                                            wasm_f32x4_pmax(one, run_duration));
                run = wasm_f32x4_div(run, wasm_f32x4_add(one, wasm_f32x4_mul(run_duration, splat(policy::plan_hazard))));
                run = wasm_f32x4_div(
                    run, wasm_f32x4_add(wasm_f32x4_add(one, wasm_f32x4_mul(wasm_f32x4_mul(risk, splat(camp_risk)),
                                                                           risk_weight)),
                                        crowding));
                run = pick(zero, run, wasm_f32x4_gt(run_duration, splat(rounds_left)));
                wasm_v128_store(&lanes.run_won[i], wasm_f32x4_gt(run, value));
                value = wasm_f32x4_pmax(value, run);
            }
            value = wasm_f32x4_div(
                value, wasm_f32x4_add(one, wasm_f32x4_mul(wasm_f32x4_add(travel, waiting), splat(travel_hazard))));
            // Keep ordinary farmers in neighbouring camp regions without
            // putting another crowding charge on hard groups' planned parties.
            const v128_t isolation = pick(zero, wasm_v128_load(&lanes.cohesion[i]), hard);
            value = wasm_f32x4_div(
                value, wasm_f32x4_add(one, wasm_f32x4_mul(isolation, splat(policy::cohesion_hazard))));
            const v128_t enemy_side = wasm_v128_load(&lanes.enemy_side[i]);
            v128_t cooling_gold_detour =
                cooling_detour_runner
                    ? wasm_v128_and(enemy_side, wasm_f32x4_gt(wasm_v128_load(&lanes.gold[i]), zero))
                    : no_bits;
            cooling_gold_detour = wasm_v128_and(
                cooling_gold_detour, wasm_f32x4_eq(claimants, zero));
            cooling_gold_detour = wasm_v128_andnot(cooling_gold_detour, is_cooling);
            cooling_gold_detour = wasm_v128_and(
                cooling_gold_detour, wasm_f32x4_lt(duration, splat(cooling_window)));
            const v128_t side_factor = pick(
                splat(std::max(enemy_side_mul, policy::cooling_detour_enemy_side_factor)),
                splat(enemy_side_mul), cooling_gold_detour);
            value = pick(wasm_f32x4_mul(value, side_factor), value, enemy_side);
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
                if (lanes.path[k] != group_path[k]) {
                    lanes.path[k] = group_path[k];
                    if (partner_arrival[k] >= 0) // The partner's wait beyond the new walk.
                        lanes.partner_wait[k] = std::max(0.0f, partner_arrival[k] - float(lanes.path[k]));
                    gather_run(k, nullptr); // Its run starts from the new walk.
                    changed = true;
                }
            if (changed)
                score_lanes(i);
        }
    }
    last_plan = round;
    // The chosen group's run, when it scored above the group alone, is taken
    // (worker_farm()): its members after the group, the next last. A choice
    // of another group ends the run being taken; the same group, a member
    // on its way, keeps it.
    if (choice < 0 || choice != target)
        cluster_run.clear();
    siege_group = choice >= 0 && lanes.siege_won[choice] ? choice : -1;
    if (choice >= 0 && cluster_run.empty() && lanes.run_won[choice]) {
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
            const bool idle = bot.position == from && t >= 0 &&
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
    // A hard group's clear, once under way (its camps damaged) with us at it,
    // is seen through: re-planning every few rounds pulled champions off
    // monoliths mid-clear (48 of 371 re-plans in four monolith games, one at
    // 3,829 health of 20,000), and a clear of 50+ active rounds meets ten
    // re-plans. Going home hurt and fights still come first (the turn's
    // order).
    bool engaged = false;
    if (target >= 0 && groups[target].status == CampGroup::alive && groups[target].health < groups[target].max_health) {
        const auto &h = groups[target];
        engaged = h.live_hard && distance(position, h.centre) <= h.live_reach + 3;
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
    // A hard group not yet struck waits for a partner: out of every camp's
    // reach, until a teammate claiming it too is there (policy::
    // hard_camp_health). Camp choice weighs the wait each replan.
    const bool woken = g.attacked >= int(round) - rules::settings().camp_sleep_time - 1;
    const int far = g.live_reach;
    if (g.live_hard && !woken) {
        bool partner = false;
        for (std::size_t i = 0; i < allies.size(); ++i)
            partner = partner ||
                      (mate_claim(ally_slot[i]) == target && distance(allies[i].position, g.centre) <= far + 3);
        if (!partner) {
            auto wait = siege_tiles(g, float(far + 1), false);
            subtract(wait, blocked);
            if (!wait.is_set(position))
                move_to(wait, g.centre, false, true);
            farm_stalls_count.counted = true; // Waiting, not stalled.
            combat(false);
            ward_ahead(g.centre);
            return;
        }
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
            return mate_claim(s);
    return -1;
}

bool Bot::capture_outranked(const CampGroup &g) const {
    // A visible ally on the capture that claims it holds it instead of us
    // when it outranks us (outranks()); a nearby claiming Prism also prompts a
    // handoff. Every holder applies the same rank, so exactly one stays.
    const bool on = g.tiles().is_set(position);
    const int group = int(&g - groups.data());
    for (const auto &ally : allies) {
        if (ally.health <= 0)
            continue;
        const int claim = claimed_by(ally);
        // A non-Prism holder hands the capture to a nearby Prism claiming it,
        // including one-tile regions where Prism cannot step on first.
        const bool prism_handoff = on && type != bc::champion_type::prism &&
                                   ally.type == bc::champion_type::prism && claim == group &&
                                   distance(ally.position, g.centre) <= 3;
        if (!g.tiles().is_set(ally.position) && !prism_handoff)
            continue;
        // Only an ally that claims the camp means to hold it; one crossing
        // the tiles or retreating over them leaves the next turn.
        // ... or, on the tiles while we are not, reports a fight (a holder
        // asking for help; a fighter on our own tiles is a passer-by).
        if (claim != group && !(!on && reports_fight(ally)))
            continue;
        // One riding its capture's recall home cannot step off: it holds the
        // capture, and stepping on only halves its reward. Otherwise Prism's
        // explicit type priority chooses the sole holder.
        if (ally.channel || prism_handoff || outranks(ally, !on))
            return true;
    }
    return false;
}

bool Bot::capture_deferred(const CampGroup &g) const {
    // A teammate claiming the group that rides its capture's recall home or
    // outranks us (Prism first, then the bigger bag) holds it from the start:
    // two on the tiles split the reward (a fifth of our captures, willow
    // best girl's 2%).
    const int group = int(&g - groups.data());
    for (const auto &ally : allies)
        if (ally.health > 0 && claimed_by(ally) == group && distance(ally.position, g.centre) <= 3) {
            const bool ally_prism = ally.type == bc::champion_type::prism;
            const bool self_prism = type == bc::champion_type::prism;
            const bool nearby_outranks =
                ally_prism != self_prism ? ally_prism : ally.gems > int(gems);
            if (ally.channel || (g.tiles().is_set(ally.position) ? outranks(ally, true) : nearby_outranks))
                return true;
        }
    return false;
}

int Bot::capture_payout(const CampGroup &g) const {
    // The timer starts at the first capture phase held, no earlier than the
    // round after we stepped on, and pays capture_time phases later
    // (docs/GameRules.md: a five-round timer counted down by later phases).
    return std::max(g.holding_since, capture_since + 1) + rules::settings().capture_time;
}

bool Bot::outranks(const ObservedUnit &ally, bool arriving) const {
    // When Prism is present it owns the capture: carried gems are Focus for it,
    // whereas another holder would normally bank them soon. Among champions
    // of the same priority keep the old order: bigger bag, current holder,
    // then lower id.
    const bool ally_prism = ally.type == bc::champion_type::prism;
    const bool self_prism = type == bc::champion_type::prism;
    if (ally_prism != self_prism)
        return ally_prism;
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
    return passive != bc::passive_type::gem_focus && int(round) - left_spawn < policy::short_trip_rounds;
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
    // teammates' in view, the Prism's from team memory; enemies' as last
    // seen, alive), and the gap against what the leader still needs to win.
    // Early on the leader needs most of the race, so a gap weighs little; a
    // gap the leader is about to close weighs fully. (A gap against 80 gems
    // from round 150 on read 40 tributed gems behind at round 200 as fully
    // behind, and our Prism's unbanked bag of up to 200 as a deficit.)
    if (behind_round == int(round))
        return behind_value;
    behind_round = int(round);
    float ours = float(bc::global_submitted_gems(bc::team::ally)) + float(gems);
    bool prism_seen = passive == bc::passive_type::gem_focus;
    for (std::size_t i = 0; i < allies.size(); ++i) {
        ours += float(allies[i].gems);
        prism_seen = prism_seen || opening.roster_types[ally_slot[i]] == bc::champion_type::prism;
    }
    if (!prism_seen)
        ours += float(shared.prism_bag());
    float theirs = float(bc::global_submitted_gems(bc::team::enemy));
    for (std::size_t i = 0; i < ghost_seen.size(); ++i)
        if (ghost_seen[i] && !(i < enemy_respawn.size() && enemy_respawn[i] >= 0))
            theirs += ghost_seen[i]->tributing ? 0.0f : float(ghost_seen[i]->gems);
    const float win = float(rules::settings().winning_gems);
    behind_value = std::clamp((theirs - ours) / std::max(1.0f, win - theirs), 0.0f, 1.0f);
    return behind_value;
}

