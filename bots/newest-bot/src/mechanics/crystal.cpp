// Teleport crystal shortcuts: one hop over a wall or hole strip, bought at
// spawn and used on the way, only when it clearly beats walking around.
#include "../bot.h"

namespace {
constexpr std::uint16_t far = bc::distance_field::unreachable;
// Turns a hop costs: the channel, then the turn it lands on.
int hop_turns() { return bc::constants::tp_crystal_time + 1; }
} // namespace

void Bot::prepare_hops() {
    // Terrain is immutable, so the hop map is built once, a row at a time, from
    // fuel left over at the end of turns.
    auto &h = hops;
    if (h.stage == HopMap::ready)
        return;
    if (h.stage == HopMap::start) {
        if (bc::fuel_remaining() < policy::planning_reserve)
            return;
        h.width = bc::map::width();
        h.height = bc::map::height();
        h.walk = home_field.export_linear_buffer();
        // A bitboard's linear export packs one bit per tile; mark tiles directly.
        h.open.assign(h.walk.size(), 1);
        for (auto p : static_blocked.positions())
            h.open[p.y * h.width + p.x] = 0;
        h.nearest.assign(h.walk.size(), far);
        h.hop.assign(h.walk.size(), far);
        h.stage = HopMap::rows;
        return;
    }
    const int w = h.width, r = rules::crystal_range;
    // The nearest takeoff within crystal range of each tile: a running minimum
    // of the walking distance from spawn, along rows and then down columns.
    while (h.stage == HopMap::rows) {
        if (bc::fuel_remaining() < policy::action_reserve)
            return;
        const int base = h.row * w;
        for (int x = 0; x < w; ++x) {
            std::uint16_t m = far;
            for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k)
                m = std::min(m, h.walk[base + k]);
            h.nearest[base + x] = m;
        }
        if (++h.row == h.height) {
            h.stage = HopMap::columns;
            h.row = 0;
        }
    }
    while (h.stage == HopMap::columns) {
        if (bc::fuel_remaining() < policy::action_reserve)
            return;
        const int y = h.row;
        for (int x = 0; x < w; ++x) {
            const int i = y * w + x;
            if (!h.open[i])
                continue;
            std::uint16_t m = far;
            for (int k = std::max(0, y - r); k <= std::min(h.height - 1, y + r); ++k)
                m = std::min(m, h.nearest[k * w + x]);
            if (m == far)
                continue;
            h.hop[i] = static_cast<std::uint16_t>(m + hop_turns());
            // Islands stay out: our goals are always reachable on foot.
            if (h.walk[i] != far && h.hop[i] + policy::crystal_min_saving <= h.walk[i])
                h.shortcuts.push_back(i);
        }
        if (++h.row == h.height) {
            h.stage = HopMap::ready;
            h.nearest = {};
            h.open = {};
            if constexpr (policy::diagnostic)
                std::printf("HOPS r=%u type=%d shortcuts=%zu\n", round, int(type), h.shortcuts.size());
        }
    }
}

bool Bot::crystal_travel(int goal, const bc::bitboard &goals) {
    auto &me = bc::self();
    const auto crystal = bc::consumable_type::tp_crystal;
    const bool held = me.consumables()[crystal].count() > 0;
    const bool at_spawn = bc::map::spawn_region(bc::team::ally).is_set(position);
    const bool planning = at_spawn && hops.stage == HopMap::ready;
    // Crystals are sold only at spawn; without one there is nothing to decide.
    if (!held && !(planning && me.can_buy(crystal)))
        return false;
    if (bc::fuel_remaining() < policy::planning_reserve)
        return false;
    auto &plan = crystal_plan;
    const int w = bc::map::width();
    if (goal != plan.goal) {
        bc::distance_field field;
        bc::bitboard visited;
        field.compute(static_blocked, goals & ~static_blocked, visited);
        plan.to_goal = field.export_linear_buffer();
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
        int best = far, landing = -1;
        for (const int t : hops.shortcuts) {
            const int rest = plan.to_goal[t];
            if (rest != far && hops.hop[t] + rest < best) {
                best = hops.hop[t] + rest;
                landing = t;
            }
        }
        if (landing >= 0 && here != far && here - best >= policy::crystal_min_saving) {
            const vec2 land{landing % w, landing / w};
            vec2 takeoff = land;
            int nearest = far;
            for (auto s : (around(land, rules::crystal_range) & ~static_blocked).positions())
                if (hops.walk[s.y * w + s.x] < nearest) {
                    nearest = hops.walk[s.y * w + s.x];
                    takeoff = s;
                }
            // The hop map measures from the nearest spawn tile; price the walk
            // from this one exactly.
            bc::distance_field route;
            bc::bitboard visited;
            route.compute_to(static_blocked, around(position, 0), around(takeoff, 0), visited);
            const int walk = route.get(takeoff);
            if (walk != far) {
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
    if (!crystal.count() || to_goal.empty() || me.restrictions().contains(bc::buff_restrictions::consumable))
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
        return l.first != r.first ? l.first < r.first
                                  : l.second.y != r.second.y ? l.second.y < r.second.y : l.second.x < r.second.x;
    });
    for (int attempt = 0; attempt < 3 && attempt < int(landings.size()); ++attempt) {
        const auto [rest, landing] = landings[attempt];
        if (incoming(landing) * bc::constants::tp_crystal_time >= health * 0.5f)
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
    const int wait = bc::constants::tp_crystal_time;
    for (const auto &enemy : enemies)
        if (distance(position, enemy.position) <=
            std::max(enemy.range, enemy.spell_reach) + enemy.move_speed * wait)
            return false;
    return incoming(position) * wait < health * 0.5f;
}
