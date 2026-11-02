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
                            float(next) + incoming(choices[k]) * (urgent ? 0.06f : 0.012f);
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
            score += incoming(p) * (urgent ? 0.8f : 0.14f);
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
    // champion).
    // At a camp with no enemy in view, only that walk counts: print dont
    // int's tanks leave camps at 15-20% of their health (golems and
    // monoliths ~10%), 1-2 rounds of the camps' damage past it; ours, home
    // at 35%, made twice the trips. Applied in fights too, the walk-out
    // rule lost (vleave).
    const float share = policy::retreat_health;
    // At a camp: every awake camp reaching us is of the group we clear (one
    // of another group striking us is no clear to stay for: our Vesper
    // stood at 20% beside another group's wolf until it killed her).
    int out = 2;
    bool reached = false, ours = target >= 0;
    float reaching = 0;
    for (const auto &c : camps)
        if (c.strength > 0 && distance(position, c.position) <= c.range && !camp_dormant(c)) {
            out = std::max(out, damage::rounds_in_reach(c.range, distance(position, c.position)));
            reached = true;
            ours = ours && group_at(c.position) == target;
            reaching += c.strength;
        }
    const bool at_camp = reached && ours;
    float per_round = incoming(position);
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
        // walking out (12 camp deaths, 2 before).
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
    // Hurt is low health. The walk out under fire counts the camps' damage
    // (they do not chase, but a hit cancels a recall); the enemies' is the
    // fight code's to weigh (its exit test, too dangerous to stay), which
    // keeps our strength and is judged again each turn. Counted here, two
    // rounds of every enemy's whole blow (incoming()) made full-health
    // champions hurt beside two or three enemies: latched home, counting
    // themselves as nothing, they were chased down (with dashes counted, 3
    // times our deaths and a sixth of our kills in six games).
    const float from_camps = per_round - incoming(position, false);
    // At camps that reach us, the walk out of their reach goes under the
    // enemies' damage too, round by round: counted for the camps alone,
    // all five of ours stayed at a contested centre elk while enemies
    // poked, dropped under the low line one by one and were caught walking
    // home alone (01elk-arena: 3 deaths to 37).
    const float walking_out = reached ? per_round : from_camps;
    return health < max_health * share || health <= walking_out * float(out);
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
    // A recall reports itself as on the way home.
    if (safe && walk > recall_turns * stats.base_move_speed) {
        me.recall();
        publish(TeamMemory::homeward);
        recall_tile = position;
        return;
    }
    // Caught by enemies we cannot outrun home: stand and fight from here (no
    // dash into the group) rather than take their hits for free; nothing to
    // strike, walk on.
    if (!safe && chase_mode() == Chase::fight && combat(true, -1, range <= 1))
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
        publish(TeamMemory::homeward);
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
                value = std::min(value, distance(tile, enemy.position) - damage::standing_reach(enemy));
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

void Bot::hop_buffers() {
    // plan_hop()'s and prepare_hops()' field buffers, rows padded for SIMD.
    if (hop_goal)
        return;
    const std::size_t tiles = std::size_t(map_width) * std::size_t(map_height);
    const int reach = rules::crystal_range;
    const int stride = (map_width + 2 * reach + 7) & ~7, wide = (map_width + 7) & ~7;
    hop_goal.reset(new std::uint16_t[tiles]);
    hop_from.reset(new std::uint16_t[tiles + 8]); // plan_hop() reads a SIMD step past the end
    hop_rows.reset(new std::uint16_t[std::size_t(stride) * std::size_t(map_height) + 8]);
    hop_near.reset(new std::uint16_t[std::size_t(wide) * std::size_t(map_height)]);
}

void row_window_min(const std::uint16_t *field, int w, int h, std::uint16_t *rows, std::uint16_t *near,
                    int first_row, int last_row, int first_column, int end_column, const int *run_start,
                    const int *run_end, const int *row_runs, const std::uint16_t *run_label,
                    const std::uint16_t *allowed, int kinds) {
    // Rows padded by the range both sides, then the window minimum along
    // each row, eight tiles a SIMD step. A step's lanes past the map's edge
    // may read the next row: they are never used. With regions, a row is
    // unreachable but for its runs in an allowed region, copied whole (a
    // separate pass over the map cost 55k a plan).
    const int reach = rules::crystal_range;
    const int stride = (w + 2 * reach + 7) & ~7, wide = (w + 7) & ~7;
    constexpr std::uint16_t far = bc::distance_field::unreachable;
    for (int y = first_row; y <= last_row; ++y) {
        std::uint16_t *row = rows + std::size_t(y) * std::size_t(stride);
        std::fill(row, row + reach, far);
        const std::uint16_t *from = field + std::size_t(y) * std::size_t(w);
        if (!run_start) {
            std::memcpy(row + reach, from, std::size_t(w) * 2);
        } else {
            std::fill(row + reach, row + reach + w, far);
            for (int r = row_runs[y]; r < row_runs[y + 1]; ++r)
                if (std::find(allowed, allowed + kinds, run_label[r]) != allowed + kinds)
                    std::memcpy(row + reach + run_start[r], from + run_start[r],
                                std::size_t(run_end[r] - run_start[r]) * 2);
        }
        std::fill(row + reach + w, row + stride, far);
    }
    std::fill(rows + std::size_t(stride) * std::size_t(h), rows + std::size_t(stride) * std::size_t(h) + 8, far);
    // Each step's eight tiles and the next eight are loaded once (the next
    // step's first eight), the window's other six views shuffled out of the
    // two: a SIMD load costs about seven fuel, a shuffle one (seven loads a
    // step were most of a hop plan's row pass). Lanes past the row read at
    // most 13 entries on, inside the padding.
    static_assert(rules::crystal_range == 3, "the views below are a 7-tile window");
    for (int y = first_row; y <= last_row; ++y) {
        const std::uint16_t *row = rows + std::size_t(y) * std::size_t(stride);
        std::uint16_t *out = near + std::size_t(y) * std::size_t(wide);
        v128_t here = wasm_v128_load(row + first_column);
        for (int x = first_column; x < end_column; x += 8) {
            const v128_t next = wasm_v128_load(row + x + 8);
            v128_t least = wasm_u16x8_min(here, wasm_i16x8_shuffle(here, next, 1, 2, 3, 4, 5, 6, 7, 8));
            least = wasm_u16x8_min(least, wasm_i16x8_shuffle(here, next, 2, 3, 4, 5, 6, 7, 8, 9));
            least = wasm_u16x8_min(least, wasm_i16x8_shuffle(here, next, 3, 4, 5, 6, 7, 8, 9, 10));
            least = wasm_u16x8_min(least, wasm_i16x8_shuffle(here, next, 4, 5, 6, 7, 8, 9, 10, 11));
            least = wasm_u16x8_min(least, wasm_i16x8_shuffle(here, next, 5, 6, 7, 8, 9, 10, 11, 12));
            least = wasm_u16x8_min(least, wasm_i16x8_shuffle(here, next, 6, 7, 8, 9, 10, 11, 12, 13));
            wasm_v128_store(out + x, least);
            here = next;
        }
    }
}

void hop_walks(const std::uint16_t *walk, const bc::bitboard &open_terrain, int w, int h, std::uint16_t *out) {
    // From any tile the hop lands on an open tile within crystal_range after
    // the crystal's channel, and the walk goes on from there: the nearest
    // walk within reach of each tile (along rows, then columns, eight tiles
    // a SIMD step), plus the channel, then a search from those landings in
    // their order. Tile by tile it cost 1,000 fuel a tile (2.7M on lanes,
    // 7M+ on adgato-gemcombat5). Working arrays are built once a game and
    // never freed (a freed block made later allocations dearer).
    constexpr std::uint16_t far = bc::distance_field::unreachable;
    const int reach = rules::crystal_range, channel = rules::settings().tp_crystal_time;
    const int stride = (w + 2 * reach + 7) & ~7, wide = (w + 7) & ~7;
    // The search runs on rows padded by a closed tile each side and a closed
    // row above and below (no bounds checks), the open tiles as bytes. hop:
    // each tile's value; held: the same with closed tiles 0 (what a
    // neighbour can be lowered from).
    const int pw = w + 2;
    const std::size_t padded = std::size_t(pw) * std::size_t(h + 2) + 16;
    static std::unique_ptr<std::uint16_t[]> rows, near, hop, held, landing_value;
    static std::unique_ptr<std::uint8_t[]> open, open_bits;
    static std::unique_ptr<int[]> queue, landing_tile;
    if (!hop) {
        rows.reset(new std::uint16_t[std::size_t(stride) * std::size_t(h) + 8]);
        near.reset(new std::uint16_t[std::size_t(wide) * std::size_t(h)]);
        hop.reset(new std::uint16_t[padded]);
        held.reset(new std::uint16_t[padded]);
        open.reset(new std::uint8_t[padded]);
        open_bits.reset(new std::uint8_t[bc::bitboard::linear_buffer_size()]);
        queue.reset(new int[padded]);
        landing_tile.reset(new int[padded]);
        landing_value.reset(new std::uint16_t[padded]);
    }
    row_window_min(walk, w, h, rows.get(), near.get(), 0, h - 1, 0, wide);
    open_terrain.export_linear_buffer(std::span(open_bits.get(), bc::bitboard::linear_buffer_size()));
    std::uint8_t *o = open.get();
    std::memset(o, 0, padded);
    for (int y = 0, index = 0; y < h; ++y) {
        std::uint8_t *row = o + std::size_t(y + 1) * std::size_t(pw) + 1;
        for (int x = 0; x < w; ++x, ++index)
            row[x] = open_bits[std::size_t(index) >> 3] >> (index & 7) & 1;
    }
    // Along the columns: each open tile's landing value (far: none). A row's
    // last step writes past its end into padding the next row rewrites; the
    // padding is reset below.
    const v128_t far8 = wasm_u16x8_splat(far), zero = wasm_u16x8_splat(0),
                 add = wasm_u16x8_splat(std::uint16_t(channel));
    for (int y = 0; y < h; ++y) {
        const int top = std::max(0, y - reach), bottom = std::min(h - 1, y + reach);
        for (int x = 0; x < wide; x += 8) {
            v128_t least = wasm_v128_load(near.get() + std::size_t(top) * std::size_t(wide) + std::size_t(x));
            for (int k = top + 1; k <= bottom; ++k)
                least = wasm_u16x8_min(least,
                                       wasm_v128_load(near.get() + std::size_t(k) * std::size_t(wide) + std::size_t(x)));
            const std::size_t p = std::size_t(y + 1) * std::size_t(pw) + 1 + std::size_t(x);
            const v128_t open_lanes = wasm_i16x8_ne(wasm_u16x8_load8x8(o + p), zero);
            const v128_t valid = wasm_v128_and(wasm_i16x8_ne(least, far8), open_lanes);
            const v128_t value = wasm_v128_bitselect(wasm_i16x8_add(least, add), far8, valid);
            wasm_v128_store(hop.get() + p, value);
            wasm_v128_store(held.get() + p, wasm_v128_bitselect(value, zero, open_lanes));
        }
    }
    for (int y = 0; y < h + 2; ++y) {
        const std::size_t left = std::size_t(y) * std::size_t(pw), right = left + std::size_t(w) + 1;
        hop[left] = hop[right] = far;
        held[left] = held[right] = 0;
    }
    std::fill(hop.get(), hop.get() + pw, far);
    std::fill(held.get(), held.get() + pw, std::uint16_t(0));
    std::fill(hop.get() + std::size_t(h + 1) * std::size_t(pw), hop.get() + padded, far);
    std::fill(held.get() + std::size_t(h + 1) * std::size_t(pw), held.get() + padded, std::uint16_t(0));
    // The landings the search starts from, in order of value: only those
    // with a neighbour above their value plus one (eight tiles a SIMD step).
    // Any other one lowers nothing when its turn comes, so leaving it out
    // changes no value.
    const int offsets[8] = {-pw - 1, -pw, -pw + 1, -1, 1, pw - 1, pw, pw + 1};
    const v128_t one = wasm_u16x8_splat(1);
    std::size_t landings = 0;
    std::uint16_t top_value = 0;
    for (int y = 1; y <= h; ++y)
        for (int x = 1; x <= w; x += 8) {
            const std::size_t p = std::size_t(y) * std::size_t(pw) + std::size_t(x);
            const v128_t value = wasm_v128_load(hop.get() + p);
            v128_t above = wasm_v128_load(held.get() + p + offsets[0]);
            for (int k = 1; k < 8; ++k)
                above = wasm_u16x8_max(above, wasm_v128_load(held.get() + p + offsets[k]));
            const v128_t lowers =
                wasm_v128_and(wasm_u16x8_gt(above, wasm_i16x8_add(value, one)), wasm_i16x8_ne(value, far8));
            unsigned lanes = unsigned(wasm_i16x8_bitmask(lowers)) & ((1u << std::min(8, w + 1 - x)) - 1);
            for (; lanes; lanes &= lanes - 1) {
                const std::size_t t = p + unsigned(__builtin_ctz(lanes));
                landing_tile[landings] = int(t);
                landing_value[landings++] = hop[t];
                top_value = std::max(top_value, hop[t]);
            }
        }
    // Counted by value into order (the queue's far end as the scratch list).
    static std::vector<int> landing_count;
    landing_count.assign(std::size_t(top_value) + 2, 0);
    for (std::size_t i = 0; i < landings; ++i)
        ++landing_count[std::size_t(landing_value[i]) + 1];
    for (std::size_t v = 1; v < landing_count.size(); ++v)
        landing_count[v] += landing_count[v - 1];
    int *order = queue.get() + (padded - landings);
    for (std::size_t i = 0; i < landings; ++i)
        order[landing_count[landing_value[i]]++] = landing_tile[i];
    for (std::size_t i = 0; i < landings; ++i) {
        landing_tile[i] = order[i];
        landing_value[i] = hop[std::size_t(order[i])];
    }
    // A search from the landings: the queue and the landings in order, the
    // lower value first, so each tile's first value is its least. A tile is
    // queued once (its first lowering is its least).
    std::size_t head = 0, tail = 0, next = 0;
    std::uint16_t *values = hop.get();
    for (;;) {
        std::size_t t;
        if (head < tail && (next == landings || values[queue[head]] <= landing_value[next]))
            t = std::size_t(queue[head++]);
        else if (next < landings) {
            t = std::size_t(landing_tile[next]);
            if (values[t] < landing_value[next++])
                continue; // Reached on foot from a better landing: queued.
        } else
            break;
        const std::uint16_t step = std::uint16_t(values[t] + 1);
        for (const int offset : offsets) {
            const std::size_t n = t + std::size_t(std::ptrdiff_t(offset));
            if (o[n] && values[n] > step) {
                values[n] = step;
                queue[tail++] = int(n);
            }
        }
    }
    for (int y = 0; y < h; ++y)
        std::memcpy(out + std::size_t(y) * std::size_t(w), values + std::size_t(y + 1) * std::size_t(pw) + 1,
                    std::size_t(w) * 2);
}

int hop_leg(const std::uint16_t *from, const std::uint16_t *to, int w, int h, int x0, int y0, int x1, int y1) {
    // Over the tiles of the box x0..x1, y0..y1 (a leg's ends and a margin:
    // a hop that saves rounds runs near the straight line), the walk to the
    // tile from `from`'s roots, the channel, and the least of `to` within
    // crystal_range of it (the best landing; a closed tile is unreachable in
    // `to`). Along each row of the box and within the range of it, only the
    // box's columns and the range either side are copied (row_window_min()
    // copies whole rows: 49 fuel a box tile on 01big-world); the window's
    // least is then taken along the row and down the columns, eight tiles
    // a SIMD step. `from` is read up to seven entries past its last tile
    // (the caller pads it).
    constexpr std::uint16_t far = bc::distance_field::unreachable;
    constexpr int reach = rules::crystal_range;
    static_assert(reach == 3, "the row views below are a 7-tile window");
    x0 = std::max(0, x0), y0 = std::max(0, y0), x1 = std::min(w - 1, x1), y1 = std::min(h - 1, y1);
    const int first_row = std::max(0, y0 - reach), last_row = std::min(h - 1, y1 + reach);
    // The box's columns, a multiple of eight wide (lanes past x1 are real
    // tiles or masked below), and its rows' window least, row by row.
    const int span = (x1 - x0 + 8) & ~7, rows_used = last_row - first_row + 1;
    static std::vector<std::uint16_t> segment, near;
    if (segment.size() < std::size_t(span + 2 * reach + 8))
        segment.resize(std::size_t(span + 2 * reach + 8));
    if (near.size() < std::size_t(span) * std::size_t(rows_used))
        near.resize(std::size_t(span) * std::size_t(rows_used));
    std::uint16_t *seg = segment.data();
    for (int y = first_row; y <= last_row; ++y) {
        // to[y][x0 - reach .. x0 + span + reach), far off the map.
        const int left = x0 - reach, right = x0 + span + reach;
        const int copy_from = std::max(0, left), copy_to = std::min(w, right);
        std::fill(seg, seg + (copy_from - left), far);
        std::memcpy(seg + (copy_from - left), to + std::size_t(y) * std::size_t(w) + std::size_t(copy_from),
                    std::size_t(copy_to - copy_from) * 2);
        std::fill(seg + (copy_to - left), seg + (right - left) + 8, far);
        std::uint16_t *out = near.data() + std::size_t(y - first_row) * std::size_t(span);
        for (int x = 0; x < span; x += 8) {
            v128_t least = wasm_v128_load(seg + x);
            for (int k = 1; k <= 2 * reach; ++k)
                least = wasm_u16x8_min(least, wasm_v128_load(seg + x + k));
            wasm_v128_store(out + x, least);
        }
    }
    const v128_t channel = wasm_u16x8_splat(std::uint16_t(rules::settings().tp_crystal_time)),
                 far8 = wasm_u16x8_splat(far), lane = wasm_u16x8_make(0, 1, 2, 3, 4, 5, 6, 7);
    v128_t best = far8;
    for (int y = y0; y <= y1; ++y) {
        const int top = std::max(0, y - reach), bottom = std::min(h - 1, y + reach);
        for (int x = 0; x < span; x += 8) {
            v128_t least = wasm_v128_load(near.data() + std::size_t(top - first_row) * std::size_t(span) + std::size_t(x));
            for (int k = top + 1; k <= bottom; ++k)
                least = wasm_u16x8_min(
                    least, wasm_v128_load(near.data() + std::size_t(k - first_row) * std::size_t(span) + std::size_t(x)));
            const v128_t out = wasm_v128_load(from + std::size_t(y) * std::size_t(w) + std::size_t(x0 + x));
            v128_t total = wasm_u16x8_add_sat(wasm_u16x8_add_sat(out, channel), least);
            // Lanes past the map's row are the next row's tiles.
            total = wasm_v128_bitselect(total, far8,
                                        wasm_u16x8_lt(lane, wasm_u16x8_splat(std::uint16_t(w - (x0 + x)))));
            best = wasm_u16x8_min(best, total);
        }
    }
    best = wasm_u16x8_min(best, wasm_i16x8_shuffle(best, best, 4, 5, 6, 7, 0, 1, 2, 3));
    best = wasm_u16x8_min(best, wasm_i16x8_shuffle(best, best, 2, 3, 0, 1, 6, 7, 4, 5));
    best = wasm_u16x8_min(best, wasm_i16x8_shuffle(best, best, 1, 0, 3, 2, 5, 4, 7, 6));
    return int(wasm_u16x8_extract_lane(best, 0));
}

int altar_hop_walk(vec2 altar, const std::uint16_t *walk, const std::uint16_t *hop, int w, int h) {
    // The least over the altar's tribute tiles, where shorter than the walk.
    const int submit = int(rules::settings().submit_gems_range);
    int best = bc::distance_field::unreachable, walked = best;
    for (int y = std::max(0, altar.y - submit); y <= std::min(h - 1, altar.y + submit); ++y)
        for (int x = std::max(0, altar.x - submit); x <= std::min(w - 1, altar.x + submit); ++x) {
            walked = std::min(walked, int(walk[y * w + x]));
            best = std::min(best, int(hop[y * w + x]));
        }
    return best < walked ? best : int(bc::distance_field::unreachable);
}

void Bot::prepare_hops() {
    // From our spawn, the walk with one teleport-crystal hop (hop_walks()).
    // One hop shortens a walk from spawn to a group by 5+ rounds on 30 of
    // our 40 test maps, and reaches groups no walk does on 4. The choices
    // made on spawn read it (on the way, move_to()'s plan). The draft
    // measures it (initialise() takes its numbers); without them it is
    // built here, once, in the background.
    hops_ready = true;
    constexpr int far = bc::distance_field::unreachable;
    group_hops.assign(groups.size(), far);
    altar_hops.assign(altars.size(), far);
    if (draft_measured()) {
        for (std::size_t i = 0; i < groups.size(); ++i)
            group_hops[i] = opening.group_distance[i][DraftPlan::hop];
        for (std::size_t i = 0; i < altars.size(); ++i)
            altar_hops[i] = opening.altar_hop[i];
    } else {
        const int w = map_width, h = map_height;
        const std::size_t tiles = std::size_t(w) * std::size_t(h);
        hop_buffers();
        // plan_hop() rewrites both buffers later: here they hold the walk
        // from spawn and the walk with a hop.
        std::uint16_t *walk = hop_goal.get(), *hop = hop_from.get();
        home_field.export_linear_buffer(std::span(walk, tiles));
        hop_walks(walk, open_terrain, w, h, hop);
        for (std::size_t i = 0; i < groups.size(); ++i)
            for (const auto &c : groups[i].camps)
                group_hops[i] = std::min(group_hops[i], int(hop[c.tile]));
        for (std::size_t i = 0; i < altars.size(); ++i)
            altar_hops[i] = altar_hop_walk(altars[i], walk, hop, w, h);
    }
    // A group's hop walk where shorter than its walk, else unreachable.
    for (std::size_t i = 0; i < groups.size(); ++i)
        if (group_hops[i] >= groups[i].home_distance)
            group_hops[i] = far;
    for (const int walk : group_hops)
        hops_useful = hops_useful || walk != far;
    for (const int walk : altar_hops)
        hops_useful = hops_useful || walk != far;
}

bool Bot::carrying_crystal() const {
    return bc::self().consumables()[bc::consumable_type::tp_crystal].count() > 0;
}

float Bot::gold_value_now() const {
    const float scarcity = 40u * opening.map_gold_forties <= policy::scarce_map_gold ? policy::scarce_gold_boost : 1.0f;
    if (type == bc::champion_type::vesper_vale && int(round) < policy::vesper_scout_rounds)
        return policy::vesper_scout_gold_value * scarcity;
    const float progress = std::min(1.0f, round / float(std::max(1u, rules::settings().max_rounds / 2)));
    // As much more as a gold makes us stronger than it makes a teammate:
    // each shard of Marcella's build adds 14 focus to a 2 x focus spell, of
    // Ryth's a share of health (gold_type_weight()).
    return (policy::early_gold_value * (1 - progress) + policy::late_gold_value * progress) * gold_type_weight() * scarcity;
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
    ensure_regions(); // Rows still unlabelled (the background labels them).
    // Over terrain: the walk from every tile to the goals (G) and from here
    // to every tile (F). A hop taking off at o costs F(o) + the channel +
    // the least G within crystal reach of o (a window minimum: along rows,
    // then columns, eight tiles a SIMD step); the least such sum against
    // G(here), the walk: taken when it saves more than `threshold` rounds.
    // A hop costs the channel at least, so a take-off farther from here than
    // the walk less the channel and the threshold never saves enough: F is
    // searched no farther, and the windows and sums run over the rows and
    // columns within that of us only. Over the whole map a plan cost 436k
    // fuel (up to 689k), freezing the champion 4-7 rounds on the ladder.
    const int w = map_width, h = map_height;
    const std::size_t tiles = std::size_t(w) * std::size_t(h);
    const int reach = rules::crystal_range, channel = rules::settings().tp_crystal_time;
    constexpr std::uint16_t far = bc::distance_field::unreachable;
    const int wide = (w + 7) & ~7;
    hop_buffers();
    // The landing regions: only tiles in the region of what we head for or one
    // beside it (hop_lands_in_reach()): a goal tile no walk joins to it (a
    // walled pocket in attack range by distance) is no landing.
    std::uint16_t allowed[9];
    int kinds = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const vec2 p{hop_plan.toward.x + dx, hop_plan.toward.y + dy};
            if (p.x < 0 || p.y < 0 || p.x >= w || p.y >= h)
                continue;
            const std::uint16_t region = walk_region_at(p.x, p.y);
            if (region && std::find(allowed, allowed + kinds, region) == allowed + kinds)
                allowed[kinds++] = region;
        }
    const auto landable = [&](std::size_t t) {
        return !kinds || std::find(allowed, allowed + kinds,
                                   walk_region_at(int(t % std::size_t(w)), int(t / std::size_t(w)))) != allowed + kinds;
    };
    // The goal side, G and M, depends only on the goals, these regions and
    // static terrain: kept for the last few goal sets, and the same goals come
    // back (a camp's respawn walked to from spawn: the same 264k plan every 40
    // rounds on tangle). A new set's first plan works its windows over the
    // band near us only; its second builds M over the map, later ones read it.
    HopGoals *entry = nullptr;
    for (auto &e : hop_cache) {
        if (e.kinds != kinds || !std::equal(allowed, allowed + kinds, e.allowed))
            continue;
        hop_cache_diff = goals;
        hop_cache_diff ^= e.goals;
        if (hop_cache_diff.is_empty()) {
            entry = &e;
            break;
        }
    }
    const v128_t far8 = wasm_u16x8_splat(far), channel8 = wasm_u16x8_splat(std::uint16_t(channel)),
                 lane = wasm_u16x8_make(0, 1, 2, 3, 4, 5, 6, 7);
    if (!entry) {
        entry = &hop_cache[hop_cache_next];
        hop_cache_next = (hop_cache_next + 1) % hop_cache.size();
        if (!entry->walk) {
            entry->walk.reset(new std::uint16_t[tiles]);
            entry->near.reset(new std::uint16_t[std::size_t(wide) * std::size_t(h)]);
        }
        entry->goals = goals;
        entry->kinds = kinds;
        entry->near_ready = false;
        std::copy(allowed, allowed + kinds, entry->allowed);
        hop_goal_field.compute(static_blocked, goals, search_visited);
        hop_goal_field.export_linear_buffer(std::span(entry->walk.get(), tiles));
    } else if (!entry->near_ready) {
        // The same goals again: M over the whole map, kept for later plans.
        // Along the rows (the padded rows masked to the regions), then down
        // each eight columns with the window's seven rows in registers.
        row_window_min(entry->walk.get(), w, h, hop_rows.get(), hop_near.get(), 0, h - 1, 0, wide,
                       kinds ? region_start.get() : nullptr, region_end.get(), region_rows.get(), region_label.get(),
                       allowed, kinds);
        const std::uint16_t *rows = hop_near.get();
        std::uint16_t *out = entry->near.get();
        const auto row_at = [&](int k, int x) {
            return k < 0 || k >= h ? far8 : wasm_v128_load(rows + std::size_t(k) * std::size_t(wide) + std::size_t(x));
        };
        for (int x = 0; x < wide; x += 8) {
            v128_t r0 = far8, r1 = far8, r2 = far8, r3 = row_at(0, x), r4 = row_at(1, x), r5 = row_at(2, x),
                   r6 = row_at(3, x);
            for (int y = 0; y < h; ++y) {
                wasm_v128_store(out + std::size_t(y) * std::size_t(wide) + std::size_t(x),
                                wasm_u16x8_min(wasm_u16x8_min(wasm_u16x8_min(r0, r1), wasm_u16x8_min(r2, r3)),
                                               wasm_u16x8_min(wasm_u16x8_min(r4, r5), r6)));
                r0 = r1;
                r1 = r2;
                r2 = r3;
                r3 = r4;
                r4 = r5;
                r5 = r6;
                r6 = row_at(y + 4, x);
            }
        }
        entry->near_ready = true;
    }
    const std::uint16_t *goal_walk = entry->walk.get(), *near = entry->near.get();
    const int walk = goal_walk[std::size_t(tile_index(position))];
    if (float(walk - channel) <= threshold)
        return;
    const int span = int(std::min(std::floor(float(walk - channel) - threshold) + 1.0f, float(far - 1)));
    hop_from_field.compute(static_blocked, around(position, 0), search_visited, std::uint16_t(span));
    hop_from_field.export_linear_buffer(std::span(hop_from.get(), tiles));
    const int first_row = std::max(0, position.y - span), last_row = std::min(h - 1, position.y + span);
    const int first_column = std::max(0, position.x - span) & ~7,
              end_column = std::min(wide, ((std::min(w - 1, position.x + span)) & ~7) + 8);
    // Each tile's sum (far past the map's edge or where either walk is), into
    // the spent padded rows at `wide`, and the least. A sum is at least its
    // take-off's walk plus the channel: a step whose every tile already costs
    // more than that lane's least so far, or than any hop that could pay (the
    // walk less the threshold), is passed (strictly more: ties stay, and the
    // first tile in row order with the least is still found below).
    std::uint16_t *sums = hop_rows.get();
    v128_t least_sum = far8;
    const v128_t pays = wasm_u16x8_splat(std::uint16_t(
        std::clamp(std::floor(float(walk) - threshold) + 1.0f, 0.0f, float(far))));
    if (entry->near_ready) {
        for (int y = first_row; y <= last_row; ++y)
            for (int x = first_column; x < end_column; x += 8) {
                const v128_t from =
                    wasm_v128_load(hop_from.get() + std::size_t(y) * std::size_t(w) + std::size_t(x));
                std::uint16_t *out = sums + std::size_t(y) * std::size_t(wide) + std::size_t(x);
                if (wasm_i16x8_all_true(
                        wasm_u16x8_gt(wasm_u16x8_add_sat(from, channel8), wasm_u16x8_min(least_sum, pays)))) {
                    wasm_v128_store(out, far8);
                    continue;
                }
                const v128_t least = wasm_v128_load(near + std::size_t(y) * std::size_t(wide) + std::size_t(x));
                const v128_t valid = wasm_v128_and(
                    wasm_u16x8_lt(lane, wasm_u16x8_splat(std::uint16_t(w - x))),
                    wasm_v128_and(wasm_i16x8_ne(least, far8), wasm_i16x8_ne(from, far8)));
                const v128_t sum =
                    wasm_v128_bitselect(wasm_u16x8_add_sat(wasm_u16x8_add_sat(from, channel8), least), far8, valid);
                wasm_v128_store(out, sum);
                least_sum = wasm_u16x8_min(least_sum, sum);
            }
    } else {
        // The band's rows (and the window's reach beyond), then down each
        // eight columns with the window's seven rows in registers (rows off
        // the map are far). The order only changes which steps the test
        // passes, never the least or the first tile with it.
        row_window_min(goal_walk, w, h, hop_rows.get(), hop_near.get(), std::max(0, first_row - reach),
                       std::min(h - 1, last_row + reach), first_column, end_column,
                       kinds ? region_start.get() : nullptr, region_end.get(), region_rows.get(), region_label.get(),
                       allowed, kinds);
        const std::uint16_t *band = hop_near.get();
        const auto near_row = [&](int k, int x) {
            return k < 0 || k >= h ? far8 : wasm_v128_load(band + std::size_t(k) * std::size_t(wide) + std::size_t(x));
        };
        for (int x = first_column; x < end_column; x += 8) {
            const v128_t in_map = wasm_u16x8_lt(lane, wasm_u16x8_splat(std::uint16_t(w - x)));
            v128_t r0 = near_row(first_row - 3, x), r1 = near_row(first_row - 2, x), r2 = near_row(first_row - 1, x),
                   r3 = near_row(first_row, x), r4 = near_row(first_row + 1, x), r5 = near_row(first_row + 2, x),
                   r6 = near_row(first_row + 3, x);
            for (int y = first_row; y <= last_row; ++y) {
                const v128_t from =
                    wasm_v128_load(hop_from.get() + std::size_t(y) * std::size_t(w) + std::size_t(x));
                std::uint16_t *out = sums + std::size_t(y) * std::size_t(wide) + std::size_t(x);
                if (wasm_i16x8_all_true(
                        wasm_u16x8_gt(wasm_u16x8_add_sat(from, channel8), wasm_u16x8_min(least_sum, pays)))) {
                    wasm_v128_store(out, far8);
                } else {
                    const v128_t least = wasm_u16x8_min(
                        wasm_u16x8_min(wasm_u16x8_min(r0, r1), wasm_u16x8_min(r2, r3)),
                        wasm_u16x8_min(wasm_u16x8_min(r4, r5), r6));
                    const v128_t valid = wasm_v128_and(
                        in_map, wasm_v128_and(wasm_i16x8_ne(least, far8), wasm_i16x8_ne(from, far8)));
                    const v128_t sum = wasm_v128_bitselect(
                        wasm_u16x8_add_sat(wasm_u16x8_add_sat(from, channel8), least), far8, valid);
                    wasm_v128_store(out, sum);
                    least_sum = wasm_u16x8_min(least_sum, sum);
                }
                if (y == last_row)
                    break;
                r0 = r1;
                r1 = r2;
                r2 = r3;
                r3 = r4;
                r4 = r5;
                r5 = r6;
                r6 = near_row(y + 4, x);
            }
        }
    }
    alignas(16) std::uint16_t lanes[8];
    wasm_v128_store(lanes, least_sum);
    int best = far;
    for (const std::uint16_t value : lanes)
        best = std::min(best, int(value));
    if (best == far || float(walk - best) <= threshold)
        return;
    // The take-off: the first tile in row order with the least sum.
    int best_tile = -1;
    const v128_t best8 = wasm_u16x8_splat(std::uint16_t(best));
    for (int y = first_row; y <= last_row && best_tile < 0; ++y)
        for (int x = first_column; x < end_column; x += 8)
            if (const unsigned hits = unsigned(wasm_i16x8_bitmask(
                    wasm_i16x8_eq(wasm_v128_load(sums + std::size_t(y) * std::size_t(wide) + std::size_t(x)), best8)))) {
                best_tile = y * w + x + __builtin_ctz(hits);
                break;
            }
    // The landing: the least G within reach of the take-off.
    const vec2 takeoff{best_tile % w, best_tile / w};
    int least = far;
    for (int y = std::max(0, takeoff.y - reach); y <= std::min(h - 1, takeoff.y + reach); ++y)
        for (int x = std::max(0, takeoff.x - reach); x <= std::min(w - 1, takeoff.x + reach); ++x)
            if (goal_walk[std::size_t(y * w + x)] < least && landable(std::size_t(y * w + x))) {
                least = goal_walk[std::size_t(y * w + x)];
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
    if (take_hop())
        return 1;
    hop_plan.landing = {-1, -1}; // Not now: the walk, for this goal.
    return -1;
}

bool Bot::hop_lands_in_reach(vec2 landing) const {
    // In the walking region of the tile the hop heads for or one beside it
    // (a group's centre can be a wall, a camp's tile is open): a walled pocket
    // in attack range of a group by distance alone was landed in, the camps
    // out of sight and no walk out (00quadrants: a Vesper stood 262 rounds).
    const std::uint16_t region = walk_region_at(landing.x, landing.y);
    if (!region)
        return false;
    const vec2 toward = hop_plan.toward;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const vec2 p{toward.x + dx, toward.y + dy};
            if (p.x >= 0 && p.y >= 0 && p.x < map_width && p.y < map_height &&
                walk_region_at(p.x, p.y) == region)
                return true;
        }
    return false;
}

bool Bot::take_hop() {
    ensure_regions();
    const vec2 landing = hop_plan.landing;
    const int reach = rules::crystal_range;
    auto &me = bc::self();
    if (me.restrictions().contains(bc::buff_restrictions::consumable))
        return false;
    // The landing, else a tile beside it in reach: a hidden occupant blocks
    // a landing without spending the crystal. Not under fire that would
    // kill us over the channel, where we stand (damage does not stop it, a
    // stun does), with our bag's margin (a round of those hits per
    // exit_bag_gems gems, as leaving weighs it); it was half our health,
    // from the landing.
    const float hits = incoming(position);
    if (hits * (float(rules::settings().tp_crystal_time) + float(gems) / float(policy::exit_bag_gems)) >=
        health + self_shield())
        return false;
    auto crystal = me.consumables()[bc::consumable_type::tp_crystal];
    const auto beside = around(landing, 1) & around(position, float(reach)) & open_terrain;
    std::vector<vec2> landings{landing};
    for (const vec2 p : beside.positions())
        if (p != landing)
            landings.push_back(p);
    for (const vec2 destination : landings) {
        if (blocked.is_set(destination) || !hop_lands_in_reach(destination))
            continue;
        if (crystal.use(destination))
            return true;
        blocked |= bc::vision::champion_tiles();
        blocked.set(destination);
        blocked.set(position, false);
    }
    return false;
}
