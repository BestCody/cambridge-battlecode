// Teleport crystal shortcuts: one hop over a wall or hole strip, bought at
// spawn and used on the way, only when it clearly beats walking around.
#include "../bot.h"
#include <wasm_simd128.h>

namespace {
constexpr std::uint16_t far = bc::distance_field::unreachable;
// Turns a hop costs: the channel alone. Measured: a hop begun on turn r lands
// at the start of turn r + 3, and the champion moves that turn.
int hop_turns() { return rules::settings().tp_crystal_time; }

// Rows are read and written eight distances at a time, the last group of a
// row running past its end: each array has this much room past its last row,
// and what a group writes past a row is rewritten with the next row.
constexpr int slack = 8;

// The minimum of each value's window [x - radius, x + radius], clamped to the
// row: from a copy padded with `far` on both sides (which leaves the clamped
// ends their minimum), each group of eight the least of the window's shifted
// loads. `in` is read, and `out` written, up to seven values past count; `row`
// holds count + 2 * radius + slack values.
void window_min(const std::uint16_t *in, std::uint16_t *out, std::uint16_t *row, int count, int radius) {
    const std::size_t size = std::size_t(count) + 2 * std::size_t(radius) + slack;
    for (int i = 0; i < radius; ++i)
        row[i] = far;
    // Copied a vector at a time: a bulk copy costs a unit of fuel per byte.
    for (int x = 0; x < count; x += 8)
        wasm_v128_store(row + radius + x, wasm_v128_load(in + x));
    for (std::size_t i = std::size_t(radius) + count; i < size; ++i)
        row[i] = far;
    for (int x = 0; x < count; x += 8) {
        const std::uint16_t *window = row + x;
        v128_t least = wasm_v128_load(window);
        for (int k = 1; k <= 2 * radius; ++k)
            least = wasm_u16x8_min(least, wasm_v128_load(window + k));
        wasm_v128_store(out + x, least);
    }
}
} // namespace

void Bot::prepare_hops() {
    // Terrain is immutable, so the hop map is built once, over the early game:
    // a fixed amount of work per turn, whatever the fuel left.
    auto &h = hops;
    if (h.stage == HopMap::ready)
        return;
    if (h.stage == HopMap::start) {
        h.width = bc::map::width();
        h.height = bc::map::height();
        const auto tiles = std::size_t(h.width) * std::size_t(h.height);
        h.walk.reset(new std::uint16_t[tiles + slack]);
        home_field.export_linear_buffer(std::span(h.walk.get(), tiles));
        std::fill_n(h.walk.get() + tiles, slack, far);
        // The row and column passes write every entry of these but the room
        // past the last row, which only the groups running past it read.
        const std::size_t span = tiles + slack,
                          row_room = std::size_t(h.width) + 2 * std::size_t(rules::crystal_range) + slack,
                          marks = std::size_t(h.height) * ((h.width + 7) / 8 + 1);
        h.work.reset(new std::uint16_t[2 * span + row_room + (marks + 1) / 2]);
        h.nearest = h.work.get();
        h.hop = h.nearest + span;
        h.padded = h.hop + span;
        h.sooner = reinterpret_cast<std::uint8_t *>(h.padded + row_room);
        std::fill_n(h.nearest + tiles, slack, far);
        std::fill_n(h.hop + tiles, slack, far);
        h.stage = HopMap::rows;
        h.row = 0;
        return;
    }
    const int w = h.width, height = h.height, r = rules::crystal_range;
    const int per_turn = int(policy::hop_tiles_per_turn);
    if (h.stage == HopMap::rows) {
        // The nearest takeoff within crystal range of each tile: a running
        // minimum of the walking distance from spawn along each row...
        for (int lines = std::max(1, per_turn / w); lines > 0 && h.row < height; --lines, ++h.row)
            window_min(&h.walk[h.row * w], &h.nearest[h.row * w], h.padded, w, r);
        if (h.row == height) {
            h.stage = HopMap::columns;
            h.row = 0;
        }
        return;
    }
    if (h.stage == HopMap::columns) {
        // ...and then down each column. The pace is counted in columns, but
        // the work is done a row at a time, which vectorises: by the end of
        // `row` of the w columns' turns, the first row * height / w rows.
        const int done = h.row;
        h.row = std::min(w, h.row + std::max(1, per_turn / height));
        const int last = h.row == w ? height : int(std::int64_t(h.row) * height / w);
        constexpr int window = 2 * rules::crystal_range + 1;
        for (int y = int(std::int64_t(done) * height / w); y < last; ++y) {
            const int top = std::max(0, y - r), bottom = std::min(height - 1, y + r);
            std::uint16_t *out = &h.hop[y * w];
            // Each group of eight: the least of its column's lines in range.
            // Away from the edges every line of the window is: a fixed count,
            // which unrolls.
            if (bottom - top + 1 == window) {
                const std::uint16_t *lines[window];
                for (int k = 0; k < window; ++k)
                    lines[k] = &h.nearest[(top + k) * w];
                for (int x = 0; x < w; x += 8) {
                    v128_t least = wasm_v128_load(lines[0] + x);
                    for (int k = 1; k < window; ++k)
                        least = wasm_u16x8_min(least, wasm_v128_load(lines[k] + x));
                    wasm_v128_store(out + x, least);
                }
                continue;
            }
            for (int x = 0; x < w; x += 8) {
                const std::uint16_t *column = &h.nearest[top * w + x];
                v128_t least = wasm_v128_load(column);
                for (int line = top + 1; line <= bottom; ++line)
                    least = wasm_u16x8_min(least, wasm_v128_load(column += w));
                wasm_v128_store(out + x, least);
            }
        }
        if (h.row == w) {
            h.stage = HopMap::collect;
            h.row = 0;
        }
        return;
    }
    // In row-major order, the tiles a hop reaches clearly sooner than walking,
    // eight at a time, and the turns reaching each takes (far elsewhere), in
    // the nearest array, free since the columns. Walking reaches no wall, and
    // islands stay out (our goals are always reachable on foot); a saturated
    // sum never qualifies.
    const int turns = hop_turns();
    std::uint16_t *cost = h.nearest;
    const v128_t unreachable = wasm_i16x8_splat(-1),
                 margin = wasm_i16x8_splat(std::int16_t(turns + policy::crystal_min_saving)),
                 hop_cost = wasm_i16x8_splat(std::int16_t(turns));
    // A shortcut beside one costing a turn less (or the same plus the step,
    // coming first in row-major order) never wins a scan for any goal: from
    // that neighbour the goal is at most a step further. Only the others are
    // listed, so every scan still picks the landing it would have. Each row
    // is listed once the row after it is known.
    // A tile is far in `cost` exactly when no hop reaches it sooner (a sooner
    // one's turns are below far), so the marks made with the row list them.
    const int groups = (w + 7) / 8;
    const v128_t one = wasm_i16x8_splat(1);
    const auto list_row = [&](int r) {
        const std::uint8_t *marks = &h.sooner[std::size_t(r) * (groups + 1)];
        if (!marks[groups])
            return;
        const std::uint16_t *row = &cost[r * w];
        const std::uint16_t *up = r > 0 ? row - w : nullptr, *down = r + 1 < height ? row + w : nullptr;
        for (int g = 0; g < groups; ++g) {
            if (!marks[g])
                continue;
            // The group's eight tiles at once against each neighbour: one
            // before it in row-major order (left, up) skips it when a turn
            // cheaper or equal plus the step, one after it (right, down) when
            // strictly cheaper. A missing neighbour is far, which never does;
            // the last tile's right neighbour is past the row.
            const int x = 8 * g;
            const v128_t here = wasm_v128_load(row + x);
            const v128_t left = x > 0 ? wasm_v128_load(row + x - 1)
                                      : wasm_i16x8_shuffle(unreachable, here, 7, 8, 9, 10, 11, 12, 13, 14);
            const v128_t right = wasm_v128_load(row + x + 1);
            const v128_t above = up ? wasm_v128_load(up + x) : unreachable;
            const v128_t below = down ? wasm_v128_load(down + x) : unreachable;
            const auto before = [&](v128_t v) {
                return wasm_v128_and(wasm_i16x8_ne(v, unreachable), wasm_u16x8_le(wasm_i16x8_add(v, one), here));
            };
            const auto after = [&](v128_t v) {
                return wasm_v128_and(wasm_i16x8_ne(v, unreachable), wasm_u16x8_lt(wasm_i16x8_add(v, one), here));
            };
            const unsigned right_lanes = x + 8 < w ? 0xffu : (1u << (w - x - 1)) - 1;
            const unsigned skipped =
                wasm_i16x8_bitmask(wasm_v128_or(wasm_v128_or(before(left), before(above)), after(below))) |
                (wasm_i16x8_bitmask(after(right)) & right_lanes);
            for (unsigned lanes = marks[g] & ~skipped; lanes; lanes &= lanes - 1) {
                const int at = x + __builtin_ctz(lanes);
                h.shortcuts.push_back(r * w + at);
                h.shortcut_turns.push_back(row[at]);
            }
        }
    };
    // The last group's lanes past the row are never marked.
    const unsigned last_lanes = w % 8 ? (1u << (w % 8)) - 1 : 0xffu;
    for (int lines = std::max(1, per_turn / w); lines > 0 && h.row < height; --lines, ++h.row) {
        const int start = h.row * w;
        const std::uint16_t *hop = &h.hop[start], *walk = &h.walk[start];
        std::uint16_t *out = &cost[start];
        std::uint8_t *marks = &h.sooner[std::size_t(h.row) * (groups + 1)];
        unsigned any = 0;
        for (int g = 0; g < groups; ++g) {
            const int x = 8 * g;
            const v128_t on_foot = wasm_v128_load(walk + x), hops = wasm_v128_load(hop + x);
            const v128_t sooner = wasm_v128_and(wasm_u16x8_le(wasm_u16x8_add_sat(hops, margin), on_foot),
                                                wasm_i16x8_ne(on_foot, unreachable));
            wasm_v128_store(out + x,
                            wasm_v128_bitselect(wasm_i16x8_add(hops, hop_cost), unreachable, sooner));
            const unsigned lanes = wasm_i16x8_bitmask(sooner) & (g + 1 < groups ? 0xffu : last_lanes);
            marks[g] = std::uint8_t(lanes);
            any |= lanes;
        }
        marks[groups] = std::uint8_t(any != 0);
        if (h.row > 0)
            list_row(h.row - 1);
    }
    if (h.row == height) {
        list_row(height - 1);
        h.stage = HopMap::ready;
        // The working block stays: freed, it would leave a hole among later
        // allocations, which made every turn's allocations dearer.
        if constexpr (policy::diagnostic)
            std::printf("HOPS r=%u type=%d shortcuts=%zu\n", round, int(type), h.shortcuts.size());
    }
}

bool Bot::crystal_travel(int goal, const bc::bitboard &goals) {
    profiling::Scope scope(profiling::Stage::crystal);
    auto &me = bc::self();
    const auto crystal = bc::consumable_type::tp_crystal;
    const bool held = me.consumables()[crystal].count() > 0;
    const bool at_spawn = bc::map::spawn_region(bc::team::ally).is_set(position);
    const bool planning = at_spawn && hops.stage == HopMap::ready;
    // Crystals are sold only at spawn; without one there is nothing to decide.
    if (!held && !(planning && me.can_buy(crystal)))
        return false;
    auto &plan = crystal_plan;
    const int w = bc::map::width();
    if (goal != plan.goal) {
        // The walk to the goal from everywhere: a group's own field or an
        // altar's route when background work has built it (the same search
        // from the same tiles), else searched now.
        const int altar = goal - int(groups.size());
        const bc::distance_field *field = nullptr;
        if (goal >= 0 && goal < int(groups.size()) && groups[goal].walk)
            field = &*groups[goal].walk;
        else if (altar >= 0 && altar < int(altar_routes.size()))
            field = &altar_routes[altar];
        else {
            search_goals.clear();
            search_goals |= goals;
            search_goals &= open_terrain;
            route_field.compute(static_blocked, search_goals, search_visited);
            field = &route_field;
        }
        const auto tiles = std::size_t(w) * std::size_t(bc::map::height());
        if (!plan.to_goal)
            plan.to_goal.reset(new std::uint16_t[tiles]);
        field->export_linear_buffer(std::span(plan.to_goal.get(), tiles));
        plan.goal = goal;
        plan.takeoff = {-1, -1};
        plan.saving = 0;
    }
    if (planning) {
        // The best single hop on the way from spawn: walk to a takeoff, hop,
        // walk the rest. Only tiles a hop reaches clearly sooner can host it.
        plan.takeoff = {-1, -1};
        plan.saving = 0;
        const int here = plan.to_goal[position.y * w + position.x];
        plan.routes.resize(groups.size() + altars.size());
        auto &route = plan.routes[goal];
        if (!route.known) {
            // The landing a hop reaches with the least hop and walk left (the
            // first such shortcut), and its takeoff: the open tile within
            // crystal range closest to spawn.
            int best = far, landing = -1;
            const std::uint16_t *to_goal = plan.to_goal.get(), *turns = hops.shortcut_turns.data();
            const int *tiles = hops.shortcuts.data();
            const std::size_t count = hops.shortcuts.size();
            std::size_t k = 0;
            if (count <= far) {
                // Eight shortcuts per step, each lane keeping its first least
                // sum and that shortcut's place; a sum saturates at far, which
                // never wins, as the plain sum past it would not.
                v128_t least = wasm_i16x8_splat(-1), first = wasm_i16x8_splat(0);
                v128_t place = wasm_i16x8_make(0, 1, 2, 3, 4, 5, 6, 7);
                const v128_t step = wasm_i16x8_splat(8);
                for (; k + 8 <= count; k += 8) {
                    const int *at = tiles + k;
                    const v128_t rest = wasm_u16x8_make(to_goal[at[0]], to_goal[at[1]], to_goal[at[2]], to_goal[at[3]],
                                                        to_goal[at[4]], to_goal[at[5]], to_goal[at[6]], to_goal[at[7]]);
                    const v128_t total = wasm_u16x8_add_sat(wasm_v128_load(turns + k), rest);
                    const v128_t better = wasm_u16x8_lt(total, least);
                    least = wasm_v128_bitselect(total, least, better);
                    first = wasm_v128_bitselect(place, first, better);
                    place = wasm_i16x8_add(place, step);
                }
                // The least sum over the lanes, then the first shortcut with it.
                std::uint16_t sums[8], places[8];
                wasm_v128_store(sums, least);
                wasm_v128_store(places, first);
                int chosen = -1;
                for (int lane = 0; lane < 8; ++lane)
                    if (sums[lane] != far &&
                        (sums[lane] < best || (sums[lane] == best && places[lane] < chosen))) {
                        best = sums[lane];
                        chosen = places[lane];
                    }
                if (chosen >= 0)
                    landing = tiles[chosen];
            }
            for (; k < count; ++k) {
                // A landing the goal is out of reach from sums past any best.
                const int total = turns[k] + to_goal[tiles[k]];
                if (total < best) {
                    best = total;
                    landing = tiles[k];
                }
            }
            route = {true, landing, best, {-1, -1}};
            if (landing >= 0) {
                const vec2 land{landing % w, landing / w};
                route.takeoff = land;
                int nearest = far;
                search_goals.clear();
                search_goals.draw_chebyshev(land, float(rules::crystal_range));
                search_goals &= open_terrain;
                // Listed in a kept buffer: a fresh list costs an allocation.
                static std::vector<vec2> around_landing;
                const std::size_t side = 2 * std::size_t(rules::crystal_range) + 1;
                if (around_landing.size() < side * side)
                    around_landing.resize(side * side);
                const std::uint32_t listed = search_goals.positions_into(around_landing);
                for (const vec2 s : std::span(around_landing.data(), listed))
                    if (hops.walk[s.y * w + s.x] < nearest) {
                        nearest = hops.walk[s.y * w + s.x];
                        route.takeoff = s;
                    }
            }
        }
        const int best = route.turns, landing = route.landing;
        if (landing >= 0 && here != far && here - best >= policy::crystal_min_saving) {
            const vec2 takeoff = route.takeoff;
            // The hop map measures from the nearest spawn tile; price the walk
            // from this one exactly, once per spawn tile (terrain never changes).
            // Without a crystal in hand only a walk short enough to save the
            // minimum leads anywhere (a purchase), so the search stops at that
            // length; -1 records a takeoff it did not reach, which settles a
            // purchase but not a hop with a crystal in hand.
            const int longest = here - (hop_turns() + plan.to_goal[landing]) - policy::crystal_min_saving;
            auto known = std::find_if(route.walks.begin(), route.walks.end(),
                                      [&](const auto &entry) { return entry.first == position; });
            if (known != route.walks.end() && known->second < 0 && held) {
                route.walks.erase(known);
                known = route.walks.end();
            }
            if (known == route.walks.end()) {
                int walk = -1;
                if (held || longest >= 0) {
                    route_field.compute_to(static_blocked, around(position, 0), origin_at(takeoff),
                                           search_visited, held ? far : static_cast<std::uint16_t>(longest));
                    walk = route_field.get(takeoff);
                    if (!held && walk == far)
                        walk = -1;
                }
                route.walks.emplace_back(position, walk);
                known = route.walks.end() - 1;
            }
            const int walk = known->second;
            if (walk >= 0 && walk != far) {
                plan.saving = here - (walk + hop_turns() + plan.to_goal[landing]);
                plan.takeoff = takeoff;
            }
        }
    }
    if (!held) {
        if (plan.saving < policy::crystal_min_saving)
            return false;
        me.buy(crystal);
        ++crystals_bought;
        if constexpr (policy::diagnostic)
            std::printf("CRYSTAL buy r=%u type=%d goal=%d saving=%d takeoff=%d,%d\n", round, int(type), goal,
                        plan.saving, plan.takeoff.x, plan.takeoff.y);
    }
    if (crystal_hop())
        return true;
    if (plan.takeoff.x < 0)
        return false;
    if (position == plan.takeoff) {
        // Unsafe or blocked here: walk the ordinary route and hop on the way
        // if another chance comes up.
        plan.takeoff = {-1, -1};
        return false;
    }
    return move_to(around(plan.takeoff, 0), false, true);
}

bool Bot::crystal_hop() {
    auto &me = bc::self();
    auto crystal = me.consumables()[bc::consumable_type::tp_crystal];
    const auto &to_goal = crystal_plan.to_goal;
    if (!crystal.count() || !to_goal || me.restrictions().contains(bc::buff_restrictions::consumable))
        return false;
    const int w = bc::map::width();
    const int here = to_goal[position.y * w + position.x];
    if (here == far)
        return false;
    std::vector<std::pair<int, vec2>> landings;
    for (auto t : (around(position, rules::crystal_range) & ~blocked).positions()) {
        const int rest = to_goal[t.y * w + t.x];
        if (rest != far && here - (hop_turns() + rest) >= policy::crystal_min_saving)
            landings.emplace_back(rest, t);
    }
    if (landings.empty() || !crystal_safe())
        return false;
    std::sort(landings.begin(), landings.end(), [](const auto &l, const auto &r) {
        return l.first != r.first         ? l.first < r.first
               : l.second.y != r.second.y ? l.second.y < r.second.y
                                          : l.second.x < r.second.x;
    });
    for (int attempt = 0; attempt < 3 && attempt < int(landings.size()); ++attempt) {
        const auto [rest, landing] = landings[attempt];
        if (incoming(landing) * rules::settings().tp_crystal_time >= health * 0.5f)
            continue;
        if (crystal.use(landing)) {
            ++crystals_used;
            if constexpr (policy::diagnostic)
                std::printf("CRYSTAL hop r=%u type=%d from=%d,%d to=%d,%d saving=%d\n", round, int(type),
                            position.x, position.y, landing.x, landing.y, here - (hop_turns() + rest));
            crystal_plan.takeoff = {-1, -1};
            return true;
        }
        // A hidden occupant refuses the landing without spending the crystal.
        blocked |= bc::vision::champion_tiles();
        blocked.set(landing);
        blocked.set(position, false);
    }
    return false;
}

bool Bot::crystal_safe() const {
    // Damage does not cancel the channel; a stun or a death does.
    const int wait = rules::settings().tp_crystal_time;
    for (const auto &enemy : enemies)
        if (distance(position, enemy.position) <=
            std::max(enemy.range, enemy.spell_reach) + enemy.move_speed * wait)
            return false;
    return incoming(position) * wait < health * 0.5f;
}
