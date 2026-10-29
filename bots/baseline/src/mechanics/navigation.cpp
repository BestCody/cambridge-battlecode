// Movement, mobility casts and the shared health-recovery retreat.
#include "../bot.h"
#include <cstring>
#include <tuple>
#include <wasm_simd128.h>

bool Bot::move_step(vec2 tile) {
    auto &me = bc::self();
    if (!me.can_move() || tile == me.position())
        return false;
    bool moved = false;
    try {
        moved = me.move_absolute(tile);
    } catch (const bc::error &error) {
        // A tile reserved since the occupancy was read (a teleport landing
        // set during this turn) is refused outright; nothing is spent.
        if (error.code() != bc::error_code::invalid_action_move_bad_destination)
            throw;
    }
    if (moved) {
        walked = true;
        position = me.position();
        refresh_blocked();
        return true;
    }
    // A refusal reveals the blocking occupant without spending the allowance.
    bc::vision::champion_tiles_into(bc::team::ally, occupancy);
    blocked |= occupancy;
    bc::vision::champion_tiles_into(bc::team::enemy, occupancy);
    blocked |= occupancy;
    bc::vision::camp_tiles_into(occupancy);
    blocked |= occupancy;
    bc::vision::teleport_tiles_into(occupancy);
    blocked |= occupancy;
    blocked.set(me.position(), false);
    return false;
}

bool Bot::cast_mobility(const bc::distance_field &field, bool urgent, int engaged_group) {
    auto &me = bc::self();
    const bool can_cast = me.can_cast();
    for (auto a : me.abilities()) {
        if (!a.is_dash() || !ready(a, can_cast))
            continue;
        const auto design = describe(a);
        if (walked && design.exclusive)
            continue;
        occupied_into(dash_blocked, dash_terrain);
        dash_blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(design.range), step_tiles);
        subtract_into(search_goals, step_tiles, blocked);
        if (design.modifier > 0 && design.radius > 0)
            drop_waking_landings(search_goals, int(design.range + design.radius), design.radius, engaged_group);
        // The landings in reading order, and their walks: from one export of
        // the field when there are many (an export costs about one read per
        // 150 tiles), else read one by one.
        const std::size_t tiles = std::size_t(map_width) * map_height;
        // Every landing is within the dash's range on both axes.
        const std::size_t span = 2 * std::size_t(static_cast<std::uint16_t>(design.range)) + 1;
        if (dash_landings.size() < span * span)
            dash_landings.resize(span * span);
        const std::uint32_t count = search_goals.positions_into(dash_landings);
        const bool exported = std::size_t(count) * 150 > tiles;
        if (exported) {
            if (!dash_walks)
                dash_walks.reset(new std::uint16_t[tiles]);
            field.export_linear_buffer(std::span(dash_walks.get(), tiles));
        }
        const auto walk_at = [&](vec2 p) { return int(exported ? dash_walks[tile_index(p)] : field.get(p)); };
        const vec2 *choices = dash_landings.data();
        const int here = walk_at(position);
        const float original = float(here);
        // A landing scores its walk plus the incoming estimate, which only
        // adds: one walking further than the best score so far cannot win, and
        // unless urgent one saving under two steps is never taken (it saves
        // less than the 1.75 asked even before its estimate). The rest, by
        // walk then reading order, as a sorted scan visits them; a tie goes
        // to the landing first in reading order.
        const int limit = urgent ? here : here - 2;
        static std::vector<std::int32_t> landing, walk;
        landing.clear();
        walk.clear();
        for (std::uint32_t k = 0; k < count; ++k)
            if (const int w = walk_at(choices[k]); w <= limit && choices[k] != position) {
                landing.push_back(std::int32_t(k));
                walk.push_back(w);
            }
        for (int attempt = 0; attempt < 3; ++attempt) {
            int best = -1;
            float score = original;
            for (int level = -1;;) {
                int next = std::numeric_limits<int>::max();
                for (const int w : walk)
                    if (w > level && w < next)
                        next = w;
                if (next == std::numeric_limits<int>::max() || float(next) > score)
                    break;
                for (std::size_t j = 0; j < landing.size() && float(next) <= score; ++j)
                    if (walk[j] == next) {
                        const int k = landing[j];
                        const float value =
                            float(next) + incoming(choices[k], engaged_group) * (urgent ? 0.06f : 0.012f);
                        if (value < score || (value == score && best >= 0 && k < best)) {
                            score = value;
                            best = k;
                        }
                    }
                level = next;
            }
            if (best < 0 || (!urgent && original - score < 1.75f))
                return false;
            if (a.use(choices[best])) {
                refresh_self();
                if (design.exclusive)
                    walked = true;
                return true;
            }
            blocked |= bc::vision::champion_tiles();
            blocked.set(choices[best]);
            blocked.set(position, false);
            // A refused landing reveals occupants: drop them from the choices.
            std::size_t kept = 0;
            for (std::size_t j = 0; j < landing.size(); ++j)
                if (!blocked.is_set(choices[landing[j]])) {
                    landing[kept] = landing[j];
                    walk[kept++] = walk[j];
                }
            landing.resize(kept);
            walk.resize(kept);
        }
    }
    return false;
}

std::span<const vec2> Bot::step_positions() {
    const std::size_t side = 2 * std::size_t(static_cast<std::uint16_t>(stats.base_move_speed)) + 1;
    if (step_list.size() < side * side)
        step_list.resize(side * side);
    return {step_list.data(), step_tiles.positions_into(step_list)};
}

bool Bot::move_to(const bc::bitboard &wanted, vec2 toward, bool urgent, bool use_dash, int engaged_group) {
    profiling::Scope scope(profiling::Stage::move);
    auto &me = bc::self();
    const auto state = me.state();
    if (state.contains(bc::champion_state::teleporting) || state.contains(bc::champion_state::recalling) ||
        state.contains(bc::champion_state::submitting_gems))
        return false;
    // The goals on open terrain, in a mask kept between calls (the caller's
    // is left as it is): one engine operation where a copy costs a new mask.
    auto &goals = move_goals;
    bc::detail::check(bc::detail::sdk_Bitboard_Intersect(wanted.ptr(), open_terrain.ptr(), goals.ptr()));
    if (goals.is_empty() || goals.is_set(position))
        return false;
    auto &field = move_field;
    auto &visited = search_visited;
    subtract_into(search_goals, goals, blocked);
    field.compute_to(blocked, search_goals, origin_at(position), visited);
    // With a crystal, the way there may be a hop (not when urgent: its
    // channel holds us still for three turns).
    if (!urgent && !walking_to_hop && carrying_crystal()) {
        const int hopped = follow_hop(goals, toward, field.get(position), use_dash, engaged_group);
        if (hopped >= 0)
            return hopped;
    }
    bool breaking_through = false;
    if (field.get(position) == bc::distance_field::unreachable) {
        // Camp bodies can seal an entire corridor. Route through destructible
        // obstacles, then clear the next blocking group instead of idling.
        auto temporary = static_blocked | bc::vision::champion_tiles() | bc::vision::teleport_tiles();
        if (avoid_round == int(round))
            temporary |= avoid_zone;
        temporary.set(position, false);
        field.compute_to(temporary, goals & ~temporary, around(position, 0), visited);
        breaking_through = field.get(position) != bc::distance_field::unreachable;
    }
    if (field.get(position) == bc::distance_field::unreachable)
        return false;
    if (breaking_through && !urgent) {
        // Only through a group we clear alone on this visit (visit_clears()).
        // Through one whose walk out a visit could pay, our banking
        // champions hit the monolith pair walling in guardians' altar, left
        // at the walk-out line and came back: 10,030 health lost to it and
        // 177 gems dropped at death in one game, print dont int's 131.
        int next_group = -1, best_distance = field.get(position);
        for (const auto &c : camps)
            if (distance(position, c.position) <= range && c.group >= 0 && visit_clears(groups[c.group])) {
                int d = field.get(c.position);
                if (d < best_distance) {
                    best_distance = d;
                    next_group = c.group;
                }
            }
        if (next_group >= 0 && combat(false, next_group))
            return true;
    }
    bool moved = false;
    if (use_dash)
        moved = cast_mobility(field, urgent, engaged_group);
    if (moved) {
        // A dash can reveal previously hidden camps and champions. Their tiles
        // must be removed before choosing the ordinary step in the same turn.
        refresh_blocked();
    }
    if (!me.can_move())
        return moved;
    for (int attempt = 0; attempt < 3; ++attempt) {
        // Reachable tiles exclude everything in blocked already.
        blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
        vec2 best = position;
        float best_score = std::numeric_limits<float>::infinity();
        // Equally short steps go toward the destination in a straight line.
        // Taken in tile order (lowest row, then lowest column), every walk
        // drifted to the low-x and low-y map edges: 7-8% of our time away
        // from spawn on an edge, willow best girl's 1-2% on any edge, and
        // flights there 12-16% against 1-5%, cornered.
        int best_line = std::numeric_limits<int>::max();
        const int remaining = field.get(position);
        for (const vec2 p : step_positions()) {
            if (p == position)
                continue;
            int d = field.get(p);
            if (d == bc::distance_field::unreachable)
                continue;
            if (!urgent && d >= remaining)
                continue;
            const bool bush = bush_tile[tile_index(p)];
            float least = float(d) * 10;
            if (bush)
                least -= 0.4f;
            const int line = (p.x - toward.x) * (p.x - toward.x) + (p.y - toward.y) * (p.y - toward.y);
            if (least > best_score || (least == best_score && line >= best_line))
                continue; // The incoming estimate only adds: this tile cannot win.
            float score = float(d) * 10;
            score += incoming(p, engaged_group) * (urgent ? 0.8f : 0.14f);
            // Respect bush cover when taking an aggressive approach.
            if (bush)
                score -= 0.4f;
            if (score < best_score || (score == best_score && line < best_line)) {
                best_score = score;
                best_line = line;
                best = p;
            }
        }
        if (best == position)
            break;
        if (move_step(best))
            return true;
        subtract_into(search_goals, goals, blocked);
        field.compute_to(blocked, search_goals, origin_at(position), visited);
    }
    return moved;
}

void Bot::drop_waking_landings(bc::bitboard &landings, int reach, float radius, int keep) {
    // A landing that strikes around it wakes the camps it reaches: our
    // Heart of Ruin's travel dashes woke camps it then left 5-6 times a
    // game (willow best girl's 1.3-1.4), and teammates walking by took
    // their fire. Only beside the camps we mean to fight: our target's and
    // the group `keep` names.
    bool near = false;
    for (const auto &c : camps)
        if (c.health > 0 && c.group != target && c.group != keep && distance(position, c.position) <= reach) {
            if (!near) {
                dash_wake.clear();
                near = true;
            }
            dash_wake.draw_chebyshev(c.position, radius);
        }
    if (near)
        subtract(landings, dash_wake);
}

bool Bot::health_recovery_needed() const {
    if (own_spawn_tile[tile_index(position)])
        return health < max_health;
    // Also home when the health left only covers the damage it is taking
    // (camp damage included) for the rounds it takes to walk out of reach of
    // the awake camps hitting us (two at least): camps do not chase, but a
    // hit cancels a recall, so leaving means walking out under their fire
    // (from beside an elk, four rounds; a monolith outranges every
    // champion). Mid-fight, a lower share.
    // At a camp with no enemy in view, only that walk counts: print dont
    // int's tanks leave camps at 15-20% of their health (golems and
    // monoliths ~10%), 1-2 rounds of the camps' damage past it; ours, home
    // at 35%, made twice the trips. Applied in fights too, the walk-out
    // rule lost 7 games of 80 (vleave).
    bool fighting = false;
    for (const auto &e : enemies)
        fighting = fighting || enemy_threatens(e, position);
    const float share = fighting ? policy::fight_retreat_health : policy::retreat_health;
    // At a camp: every awake camp reaching us is of the group we clear (one
    // of another group striking us is no clear to stay for: our Vesper
    // stood at 20% beside another group's wolf until it killed her).
    int out = 2;
    bool reached = false, ours = target >= 0;
    float reaching = 0;
    for (const auto &c : camps)
        if (c.strength > 0 && distance(position, c.position) <= c.range && !camp_dormant(c)) {
            out = std::max(out, c.range - distance(position, c.position) + 1);
            reached = true;
            ours = ours && group_at(c.position) == target;
            reaching += c.strength;
        }
    const bool at_camp = reached && ours;
    float per_round = incoming(position, target);
    if (at_camp && enemies.empty()) {
        // Every awake camp reaching us, at its full strength, hitting us or
        // not: behind the front we take nothing until the front leaves, then
        // all of it. On boss-battle our Vesper stood in a monolith pair's
        // reach behind Marcella; Marcella left, the pair turned on her (~360
        // a round) with steps still to walk, and she died.
        per_round = reaching * camp_mitigation * mitigation;
        // The walk out, counted: the fewest steps to a tile no awake camp
        // reaches (around walls and other camps' reach). Counted from each
        // camp alone, it came short: champions left on the line and died
        // walking out (12 camp deaths in 80 games, 2 before).
        constexpr int far = 8;
        if (health <= per_round * float(far)) {
            static bc::bitboard reach, open, covered;
            reach.clear();
            for (const auto &c : camps)
                if (c.strength > 0 && distance(position, c.position) <= c.range + far && !camp_dormant(c))
                    reach.draw_chebyshev(c.position, float(c.range));
            out = far;
            for (int steps = 1; steps < far; ++steps) {
                blocked.reachable_tiles_into(position, std::uint16_t(steps), open);
                covered = open;
                covered &= reach;
                if (covered.count() < open.count()) {
                    out = std::max(2, steps);
                    break;
                }
            }
        }
        // Nor less than the walk out from where we strike (camp choice's
        // price of a visit, visit_cost()): measured from where it stood, 3
        // from an elk pair, our Heart of Ruin stayed at 430, dashed in
        // beside them and died walking out.
        return health <= std::max(per_round * float(out), visit_cost(groups[std::size_t(target)]));
    }
    // Away from camps, home first when what we have would not pay that walk
    // out of our target's camps: one step outside an elk pair at 40%, ours
    // walked back in and out under their hits until it died.
    if (target >= 0 && health <= visit_cost(groups[std::size_t(target)]))
        return true;
    return health < max_health * share || health <= per_round * float(out);
}

void Bot::retreat_to_spawn() {
    auto &me = bc::self();
    const auto &spawn = own_spawn;
    if (spawn.is_set(position) ||
        me.state().intersects(bc::champion_state::recalling | bc::champion_state::teleporting |
                              bc::champion_state::submitting_gems) ||
        me.buffs().has_stun())
        return;
    track_ghosts();
    // One hit cancels recall. Include reach over the channel, including enemies
    // whose current stun can expire, rather than just their current attack tile.
    bc::bitboard threatened;
    for (const auto &camp : camps)
        // A camp whose reach we stand in after a hit is awake whatever our
        // record says: recalling there was cancelled by its next strike.
        if (camp.strength > 0 && distance(position, camp.position) <= stats.vision_radius + camp.range &&
            (!camp_dormant(camp) || (hit && distance(position, camp.position) <= camp.range)))
            threatened.draw_chebyshev(camp.position, camp.range);
    // Enemies out of sight count from their possible tile nearest us: one
    // that just stepped into a bush is still there.
    for (const auto *list : {&enemies, &ghosts})
        for (const auto &enemy : *list) {
            const auto design = enemy.champion().base_stats();
            // A dash ready within the recall closes its tiles too (a damaging
            // dash's spell reach already counts them).
            const int dash = enemy.dash_range > 0 && enemy.dash_wait >= 0 && enemy.dash_wait <= recall_turns
                                 ? int(enemy.dash_range)
                                 : 0;
            const int reach = std::max(enemy.range + dash, enemy.spell_reach) +
                              std::min(recall_turns, policy::recall_chase_turns) * design.base_move_speed;
            if (distance(position, enemy.position) <= stats.vision_radius + reach)
                threatened.draw_chebyshev(enemy.position, reach);
        }
    // A hit on the tile of our last recall cancelled it: whatever struck,
    // seen or not (a camp out of view, an enemy in a bush), would cancel the
    // next one too. Recalling again on the same tile kept champions standing
    // under those hits, often until they died, so walk home instead. So does
    // a hit where nothing in view reaches: our Prism, stepped out of sight of
    // the elks it woke, recalled under their fire and died.
    const bool cancelled =
        hit && (position == recall_tile || (incoming(position) == 0 && !threatened.is_set(position)));
    const bool safe = !threatened.is_set(position) && incoming(position) == 0 && !cancelled;
    const int walk = home_field.get(position);
    // A recall reports itself: teammates read every other recall as one
    // cancelled at the next turn (fake_recall()).
    if (safe && walk > recall_turns * stats.base_move_speed) {
        me.recall();
        publish(TeamMemory::recall);
        recall_tile = position;
        return;
    }
    // Caught by enemies we cannot outrun home: stand and fight from here (no
    // dash into the group) rather than take their hits for free; nothing to
    // strike, walk on.
    if (!safe && chase_mode(true) == Chase::fight && combat(true, -1, range <= 1))
        return;
    // around(...) & ~blocked & ~threatened, in place (no complement masks).
    auto safe_tiles = around(position, stats.vision_radius);
    subtract(safe_tiles, blocked);
    subtract(safe_tiles, threatened);
    const auto &goals = walk <= recall_turns * stats.base_move_speed || safe_tiles.is_empty() ||
                                cancelled
                            ? spawn
                            : safe_tiles;
    strike_before_step();
    const bool moved = move_to(goals, home_centre, true, true);
    if (!moved && safe) {
        // Occupancy can block a short walk home. Do not wait on that route when
        // this tile already permits the recall that fulfils the same recovery.
        me.recall();
        publish(TeamMemory::recall);
        recall_tile = position;
        return;
    }
    if (!moved && me.can_move()) {
        // A disconnected safe region or a blocked corridor must not turn
        // recovery into idling. Minimize immediate damage, then increase the
        // distance from attack/spell reach before preferring the way home.
        auto clearance = [&](vec2 tile) {
            int value = std::numeric_limits<int>::max();
            for (const auto &enemy : enemies)
                value = std::min(value,
                                 distance(tile, enemy.position) - std::max(enemy.range, enemy.spell_reach));
            for (const auto &camp : camps)
                if (camp.strength > 0)
                    value = std::min(value, distance(tile, camp.position) - camp.range);
            return value;
        };
        auto score = [&](vec2 tile) {
            return std::tuple{incoming(tile), -clearance(tile), home_field.get(tile)};
        };
        vec2 best = position;
        auto best_score = score(position);
        blocked.reachable_tiles_into(position, static_cast<std::uint16_t>(stats.base_move_speed), step_tiles);
        for (const vec2 tile : step_positions()) {
            const auto value = score(tile);
            if (value < best_score) {
                best = tile;
                best_score = value;
            }
        }
        if (best != position)
            move_step(best);
    }
    if (!spawn.is_set(position)) {
        observe_fight();
        // Ranged and unmoved, a shot costs no walk: fire back.
        combat(true, -1, !(range > 1 && !walked));
    }
}

void Bot::prepare_hops() {
    // From our spawn, the walk with one teleport-crystal hop: from any tile
    // the hop lands on an open tile within crystal_range after the
    // crystal's channel, and the walk goes on from there. One hop shortens
    // a walk from spawn to a group by 5+ rounds on 30 of our 40 test maps,
    // and reaches groups no walk does on 4. The choices made on spawn read
    // it (on the way, move_to()'s plan); built once from terrain: the
    // nearest walk within reach of each tile (along rows, then columns),
    // plus the channel, then a search from those landings in their order.
    hops_ready = true;
    const int w = map_width, h = map_height;
    const std::size_t tiles = std::size_t(w) * std::size_t(h);
    constexpr int far = bc::distance_field::unreachable;
    const int reach = rules::crystal_range, channel = rules::settings().tp_crystal_time;
    std::unique_ptr<std::uint16_t[]> walk(new std::uint16_t[tiles]), near(new std::uint16_t[tiles]),
        hop(new std::uint16_t[tiles]), landed(new std::uint16_t[tiles]);
    home_field.export_linear_buffer(std::span(walk.get(), tiles));
    TileBits open;
    open.load(open_terrain);
    for (int y = 0; y < h; ++y) {
        const std::uint16_t *row = walk.get() + std::size_t(y) * std::size_t(w);
        std::uint16_t *out = near.get() + std::size_t(y) * std::size_t(w);
        for (int x = 0; x < w; ++x) {
            std::uint16_t least = far;
            for (int k = std::max(0, x - reach), end = std::min(w - 1, x + reach); k <= end; ++k)
                least = std::min(least, row[k]);
            out[x] = least;
        }
    }
    // Each open tile's landing value (far: none), counted by value for the
    // order below.
    std::vector<int> landing_count(1, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int index = y * w + x;
            std::uint16_t least = far;
            for (int k = std::max(0, y - reach), end = std::min(h - 1, y + reach); k <= end; ++k)
                least = std::min(least, near[std::size_t(k) * std::size_t(w) + std::size_t(x)]);
            hop[index] = far;
            if (least != far && open[index]) {
                const int value = int(least) + channel;
                hop[index] = std::uint16_t(value);
                if (std::size_t(value) + 1 >= landing_count.size())
                    landing_count.resize(std::size_t(value) + 2, 0);
                ++landing_count[std::size_t(value) + 1];
            }
            landed[index] = hop[index];
        }
    for (std::size_t v = 1; v < landing_count.size(); ++v)
        landing_count[v] += landing_count[v - 1];
    std::vector<int> landings(std::size_t(landing_count.back()));
    for (std::size_t t = 0; t < tiles; ++t)
        if (hop[t] != far)
            landings[std::size_t(landing_count[hop[t]]++)] = int(t);
    // A search from the landings: the queue and the landings in order, the
    // lower value first, so each tile's first value is its least.
    std::vector<int> queue(tiles);
    std::size_t head = 0, tail = 0, next = 0;
    for (;;) {
        int t;
        if (head < tail && (next == landings.size() || hop[queue[head]] <= landed[landings[next]]))
            t = queue[head++];
        else if (next < landings.size()) {
            t = landings[next++];
            if (hop[t] < landed[t])
                continue; // Reached on foot from a better landing: queued.
        } else
            break;
        const int x = t % w, y = t / w;
        const std::uint16_t step = std::uint16_t(hop[t] + 1);
        for (int ny = std::max(0, y - 1); ny <= std::min(h - 1, y + 1); ++ny)
            for (int nx = std::max(0, x - 1); nx <= std::min(w - 1, x + 1); ++nx) {
                const int n = ny * w + nx;
                if (open[n] && hop[n] > step) {
                    hop[n] = step;
                    queue[tail++] = n;
                }
            }
    }
    // A goal's hop walk where shorter than its walk, else unreachable.
    group_hops.assign(groups.size(), far);
    for (std::size_t i = 0; i < groups.size(); ++i) {
        int best = far;
        for (const auto &c : groups[i].camps)
            best = std::min(best, int(hop[c.tile]));
        if (best < groups[i].home_distance) {
            group_hops[i] = best;
            hops_useful = true;
        }
    }
    altar_hops.assign(altars.size(), far);
    const int submit = int(rules::settings().submit_gems_range);
    for (std::size_t i = 0; i < altars.size(); ++i) {
        int best = far, walked = far;
        for (int y = std::max(0, altars[i].y - submit); y <= std::min(h - 1, altars[i].y + submit); ++y)
            for (int x = std::max(0, altars[i].x - submit); x <= std::min(w - 1, altars[i].x + submit); ++x) {
                const int t = y * w + x;
                walked = std::min(walked, int(walk[t]));
                best = std::min(best, int(hop[t]));
            }
        if (best < walked) {
            altar_hops[i] = best;
            hops_useful = true;
        }
    }
}

bool Bot::carrying_crystal() const {
    return bc::self().consumables()[bc::consumable_type::tp_crystal].count() > 0;
}

float Bot::gold_value_now() const {
    const float progress = std::min(1.0f, round / float(std::max(1u, rules::settings().max_rounds / 2)));
    return policy::early_gold_value * (1 - progress) + policy::late_gold_value * progress;
}

float Bot::hop_threshold() const {
    // A hop pays when the rounds it saves earn more than its crystal costs:
    // at our income a round so far (gold, and gems at camp choice's value of
    // a gold), over the whole game (captures pay in lumps). Hops that saved
    // one or two rounds, taken whenever they saved any, cost ~200 gold a
    // game on maps with short gaps (stampede: 21 hops) and lost 11 games of
    // 80.
    const float income =
        (earned_gold + earned_gems / gold_value_now()) / float(std::max(1u, round));
    return income > 0 ? float(bc::consumable_type::tp_crystal.cost()) / income
                      : std::numeric_limits<float>::infinity();
}

int Bot::hop_rounds(vec2 toward) const {
    if (toward != hop_plan.toward || hop_plan.landing.x < 0)
        return bc::distance_field::unreachable;
    return std::max(0, hop_plan.rounds - (int(round) - hop_plan.round));
}

void Bot::plan_hop(const bc::bitboard &goals, float threshold) {
    // Over terrain: the walk from every tile to the goals (G) and from here
    // to every tile (F). A hop taking off at o costs F(o) + the channel +
    // the least G within crystal reach of o (a window minimum: along rows,
    // then columns, eight tiles a SIMD step); the least such sum against
    // G(here), the walk: taken when it saves more than `threshold` rounds.
    const int w = map_width, h = map_height;
    const std::size_t tiles = std::size_t(w) * std::size_t(h);
    const int reach = rules::crystal_range, channel = rules::settings().tp_crystal_time;
    constexpr std::uint16_t far = bc::distance_field::unreachable;
    // Rows padded by `reach` both sides and to whole SIMD steps.
    const int stride = (w + 2 * reach + 7) & ~7, wide = (w + 7) & ~7;
    if (!hop_goal) {
        hop_goal.reset(new std::uint16_t[tiles]);
        hop_from.reset(new std::uint16_t[tiles]);
        hop_rows.reset(new std::uint16_t[std::size_t(stride) * std::size_t(h) + 8]);
        hop_near.reset(new std::uint16_t[std::size_t(wide) * std::size_t(h)]);
    }
    hop_goal_field.compute(static_blocked, goals, search_visited);
    hop_from_field.compute(static_blocked, around(position, 0), search_visited);
    hop_goal_field.export_linear_buffer(std::span(hop_goal.get(), tiles));
    hop_from_field.export_linear_buffer(std::span(hop_from.get(), tiles));
    const int walk = hop_goal[std::size_t(tile_index(position))];
    // Padded rows of G, then their window minimum along the row into
    // hop_near (row-major at `wide`).
    std::uint16_t *rows = hop_rows.get(), *near = hop_near.get();
    for (int y = 0; y < h; ++y) {
        std::uint16_t *row = rows + std::size_t(y) * std::size_t(stride);
        std::fill(row, row + reach, far);
        std::memcpy(row + reach, hop_goal.get() + std::size_t(y) * std::size_t(w), std::size_t(w) * 2);
        std::fill(row + reach + w, row + stride, far);
    }
    std::fill(rows + std::size_t(stride) * std::size_t(h), rows + std::size_t(stride) * std::size_t(h) + 8, far);
    for (int y = 0; y < h; ++y) {
        const std::uint16_t *row = rows + std::size_t(y) * std::size_t(stride);
        std::uint16_t *out = near + std::size_t(y) * std::size_t(wide);
        for (int x = 0; x < wide; x += 8) {
            v128_t least = wasm_v128_load(row + x);
            for (int k = 1; k <= 2 * reach; ++k)
                least = wasm_u16x8_min(least, wasm_v128_load(row + x + k));
            wasm_v128_store(out + x, least);
        }
    }
    // Along the columns, each tile's sum, and the least.
    int best = far, best_tile = -1;
    for (int y = 0; y < h; ++y) {
        const int top = std::max(0, y - reach), bottom = std::min(h - 1, y + reach);
        for (int x = 0; x < wide; x += 8) {
            v128_t least = wasm_v128_load(near + std::size_t(top) * std::size_t(wide) + std::size_t(x));
            for (int k = top + 1; k <= bottom; ++k)
                least = wasm_u16x8_min(least, wasm_v128_load(near + std::size_t(k) * std::size_t(wide) + std::size_t(x)));
            // F's row is unpadded: its last step reads only the row's own
            // tiles (the rest past the map's end is left out).
            alignas(16) std::uint16_t lanes[8];
            wasm_v128_store(lanes, least);
            const std::uint16_t *from = hop_from.get() + std::size_t(y) * std::size_t(w) + std::size_t(x);
            for (int k = 0; k < 8 && x + k < w; ++k) {
                if (lanes[k] == far || from[k] == far)
                    continue;
                const int sum = int(from[k]) + channel + int(lanes[k]);
                if (sum < best) {
                    best = sum;
                    best_tile = y * w + x + k;
                }
            }
        }
    }
    if (best_tile < 0 || float(walk - best) <= threshold)
        return;
    // The landing: the least G within reach of the take-off.
    const vec2 takeoff{best_tile % w, best_tile / w};
    int least = far;
    for (int y = std::max(0, takeoff.y - reach); y <= std::min(h - 1, takeoff.y + reach); ++y)
        for (int x = std::max(0, takeoff.x - reach); x <= std::min(w - 1, takeoff.x + reach); ++x)
            if (hop_goal[std::size_t(y * w + x)] < least) {
                least = hop_goal[std::size_t(y * w + x)];
                hop_plan.landing = {x, y};
            }
    hop_plan.takeoff_walk = hop_from[std::size_t(best_tile)];
    hop_plan.rounds = best;
}

int Bot::follow_hop(const bc::bitboard &goals, vec2 toward, int walk, bool use_dash, int engaged_group) {
    // A plan per goal, made again when we are later at its take-off than it
    // planned (held up or pushed off the way). A hop saves at most what the
    // walk exceeds the straight line by, plus its reach less its channel:
    // where that is within the crystal's price no search is spent.
    const int reach = rules::crystal_range, channel = rules::settings().tp_crystal_time;
    if (toward != hop_plan.toward ||
        (hop_plan.landing.x >= 0 && int(round) > hop_plan.round + hop_plan.takeoff_walk)) {
        hop_plan = {};
        hop_plan.toward = toward;
        hop_plan.round = int(round);
        int straight = std::numeric_limits<int>::max();
        for (const vec2 p : goals.positions())
            straight = std::min(straight, distance(position, p));
        const float threshold = hop_threshold();
        if (float(walk - straight + reach - channel) > threshold)
            plan_hop(goals, threshold);
    }
    if (hop_plan.landing.x < 0)
        return -1;
    if (distance(position, hop_plan.landing) > reach) {
        walking_to_hop = true;
        const bool moved = move_to(around(hop_plan.landing, float(reach)) & open_terrain, hop_plan.landing, false,
                                   use_dash, engaged_group);
        walking_to_hop = false;
        return moved;
    }
    if (take_hop(engaged_group))
        return 1;
    hop_plan.landing = {-1, -1}; // Not now: the walk, for this goal.
    return -1;
}

bool Bot::take_hop(int engaged_group) {
    const vec2 landing = hop_plan.landing;
    const int reach = rules::crystal_range;
    auto &me = bc::self();
    if (me.restrictions().contains(bc::buff_restrictions::consumable))
        return false;
    // The landing, else a tile beside it in reach: a hidden occupant blocks
    // a landing without spending the crystal. Not under fire that would
    // take half our health over the channel (a stun ends it).
    auto crystal = me.consumables()[bc::consumable_type::tp_crystal];
    const auto beside = around(landing, 1) & around(position, float(reach)) & open_terrain;
    std::vector<vec2> landings{landing};
    for (const vec2 p : beside.positions())
        if (p != landing)
            landings.push_back(p);
    for (const vec2 destination : landings) {
        if (blocked.is_set(destination) ||
            incoming(destination, engaged_group) * rules::settings().tp_crystal_time >= health * 0.5f)
            continue;
        if (crystal.use(destination))
            return true;
        blocked |= bc::vision::champion_tiles();
        blocked.set(destination);
        blocked.set(position, false);
    }
    return false;
}
