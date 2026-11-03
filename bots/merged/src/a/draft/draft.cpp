namespace eng_a {
// Draft: pick the fixed roster for this team size and write the opening plan
// to draft memory.
}  // namespace eng_a
#include "draft.h"
namespace eng_a {
}  // namespace eng_a
#include "../strategy.h"
namespace eng_a {
}  // namespace eng_a
#include "team_compositions.h"
namespace eng_a {
}  // namespace eng_a
#include <algorithm>
namespace eng_a {
}  // namespace eng_a
#include <vector>
namespace eng_a {
}  // namespace eng_a
#include <iostream>
namespace eng_a {
}  // namespace eng_a
#include <memory>
namespace eng_a {

namespace {
// Shared memory has a slot per body: every roster's bodies (the twin pick's
// two counted) fit.
constexpr bool rosters_fit() {
    for (const auto &entry : compositions::rosters) {
        const unsigned twin = (entry.picks & compositions::bit(compositions::Pick::cassi_and_issac)) ? 1 : 0;
        if (entry.size() + twin > TeamMemory::slots)
            return false;
    }
    return true;
}
static_assert(rosters_fit());

struct DraftState {
    bool ready = false;
    DraftPlan plan;

    void initialise() {
        const auto groups = bc::map::camp_groups();
        auto walls = bc::map::walls() | bc::map::holes() | bc::map::altars();
        unsigned productive = 0;
        unsigned cheap_gem_groups = 0;
        unsigned map_gold = 0;
        for (const auto &g : groups) {
            map_gold += g.gold_value();
            bool farmable = true;
            for (const auto &c : g) {
                auto stats = c.base_stats();
                // Classified per camp, not per group: three elks are a 7,200
                // health group but still farmable, whereas one monolith in a
                // group defends the whole group.
                if (stats.base_health > policy::cheap_camp_health ||
                    stats.base_strength > policy::cheap_camp_strength)
                    farmable = false;
            }
            if (g.gem_value() > 0) {
                ++productive;
                if (farmable)
                    ++cheap_gem_groups;
            }
        }
        plan.cheap_gem_groups = static_cast<std::uint8_t>(std::min(cheap_gem_groups, 255u));
        plan.map_gold_forties = static_cast<std::uint8_t>(std::min(map_gold / 40, 255u));
        plan.spacing = std::sqrt(float((~walls).count()) / std::max(1u, productive));
        measure_groups(groups);
        ready = true;
    }

    void measure_groups(const std::vector<bc::map::camp_group> &groups) {
        // The same searches a champion's initialisation would run (world.cpp),
        // and its hops (prepare_hops()).
        const auto altars = bc::map::altars().positions();
        if (groups.size() > DraftPlan::max_groups || altars.size() > DraftPlan::max_altars)
            return;
        const auto static_blocked = terrain();
        bc::distance_field home, enemy, altar;
        bc::bitboard visited;
        home.compute(static_blocked, bc::map::spawn_region(bc::team::ally), visited);
        enemy.compute(bc::map::walls() | bc::map::holes() | bc::map::altars(),
                      bc::map::spawn_region(bc::team::enemy), visited);
        altar.compute(static_blocked, altar_goals_of(altars, static_blocked), visited);
        const int w = bc::map::width(), h = bc::map::height();
        const std::size_t tiles = std::size_t(w) * std::size_t(h);
        const std::unique_ptr<std::uint16_t[]> walk(new std::uint16_t[tiles]), hop(new std::uint16_t[tiles]);
        home.export_linear_buffer(std::span(walk.get(), tiles));
        hop_walks(walk.get(), ~static_blocked, w, h, hop.get());
        for (std::size_t i = 0; i < groups.size(); ++i) {
            auto &distance = plan.group_distance[i];
            distance.fill(bc::distance_field::unreachable);
            for (const auto &c : groups[i]) {
                const auto p = c.spawn_position();
                distance[DraftPlan::home] = std::min(distance[DraftPlan::home], home.get(p));
                distance[DraftPlan::enemy] = std::min(distance[DraftPlan::enemy], enemy.get(p));
                distance[DraftPlan::altar] = std::min(distance[DraftPlan::altar], altar.get(p));
                distance[DraftPlan::hop] = std::min(distance[DraftPlan::hop], hop[std::size_t(p.y * w + p.x)]);
            }
        }
        // Legs out on the map: from each group to its leg_neighbours nearest
        // groups and its nearest altar in a straight line (the walk home is
        // the walk from spawn, priced above), the most rounds one crystal
        // hop saves (hop_leg(): the walk out, the channel, the walk on from
        // the best landing, over the box of the leg's ends). A hop moves
        // crystal_range tiles, so it saves at most the walk less the
        // straight line past that and the channel: a leg that cannot save
        // two rounds is not priced. A search with a hop from every group
        // (hop_walks()) cost 370k fuel a group.
        {
            const int submit = int(rules::settings().submit_gems_range);
            const int reach = rules::crystal_range, channel = rules::settings().tp_crystal_time;
            constexpr int far = bc::distance_field::unreachable;
            // The box a leg's hop is looked for in, past its ends: a hop's
            // start lies within the range of its landing, between the ends.
            constexpr int margin = rules::crystal_range;
            const std::size_t n = groups.size(), ends = n + altars.size();
            const std::size_t stride = tiles + 8; // hop_leg() reads 8 past the last tile.
            // Each end's tiles (a group's camps, an altar's tribute tiles)
            // and bounding box.
            std::vector<std::vector<vec2>> tiles_of(ends);
            std::vector<std::array<int, 4>> box(ends, {w, h, -1, -1});
            for (std::size_t i = 0; i < n; ++i)
                for (const auto &c : groups[i])
                    tiles_of[i].push_back(c.spawn_position());
            for (std::size_t k = 0; k < altars.size(); ++k) {
                const vec2 a = altars[k];
                for (int y = std::max(0, a.y - submit); y <= std::min(h - 1, a.y + submit); ++y)
                    for (int x = std::max(0, a.x - submit); x <= std::min(w - 1, a.x + submit); ++x)
                        tiles_of[n + k].push_back({x, y});
            }
            for (std::size_t e = 0; e < ends; ++e)
                for (const auto p : tiles_of[e])
                    box[e] = {std::min(box[e][0], p.x), std::min(box[e][1], p.y), std::max(box[e][2], p.x),
                              std::max(box[e][3], p.y)};
            // Each end's walk, once; copied out row-major only for a leg
            // that is priced (20 KB an end on a 100 x 100 map).
            std::vector<bc::distance_field> walks(ends);
            for (std::size_t e = 0; e < ends; ++e) {
                bc::bitboard sources;
                if (e < n) {
                    sources.clear();
                    for (const auto p : tiles_of[e])
                        sources.set(p, true);
                } else
                    sources = altar_goals_of({altars[e - n]}, static_blocked);
                walks[e].compute(static_blocked, sources, visited);
            }
            const std::unique_ptr<std::uint16_t[]> fields(new std::uint16_t[stride * ends]);
            std::vector<bool> copied(ends, false);
            const auto field_of = [&](std::size_t e) {
                std::uint16_t *field = fields.get() + e * stride;
                if (!copied[e]) {
                    walks[e].export_linear_buffer(std::span(field, tiles));
                    std::fill(field + tiles, field + stride, std::uint16_t(far));
                    copied[e] = true;
                }
                return static_cast<const std::uint16_t *>(field);
            };
            // The straight line between two ends: their nearest tiles.
            const auto line_between = [&](std::size_t a, std::size_t b) {
                int d = std::numeric_limits<int>::max();
                for (const auto p : tiles_of[a])
                    for (const auto q : tiles_of[b])
                        d = std::min(d, std::max(std::abs(p.x - q.x), std::abs(p.y - q.y)));
                return d;
            };
            // A leg's saving, both ways the same: worked out once.
            std::vector<std::int8_t> saved(n * n, -1);
            const auto price = [&](std::size_t i, std::size_t to, int line) {
                if (to < n && saved[std::min(i, to) * n + std::max(i, to)] >= 0)
                    return int(saved[std::min(i, to) * n + std::max(i, to)]);
                // The walk, read from the leg's own walk at our camps.
                int walked = far;
                for (const auto p : tiles_of[i])
                    walked = std::min(walked, int(walks[to].get(p)));
                int saving = 0;
                if (walked == far || walked - std::max(0, line - reach) - channel >= 2) {
                    const int hopped =
                        hop_leg(field_of(i), field_of(to), w, h, std::min(box[i][0], box[to][0]) - margin,
                                std::min(box[i][1], box[to][1]) - margin, std::max(box[i][2], box[to][2]) + margin,
                                std::max(box[i][3], box[to][3]) + margin);
                    if (hopped < walked)
                        saving = walked == far ? 30 : std::min(30, walked - hopped);
                }
                if (to < n)
                    saved[std::min(i, to) * n + std::max(i, to)] = std::int8_t(saving);
                return saving;
            };
            for (std::size_t i = 0; i < n; ++i) {
                // Its legs: the nearest groups and altar in a straight line.
                std::array<std::pair<int, std::size_t>, policy::leg_neighbours + 1> legs;
                legs.fill({std::numeric_limits<int>::max(), 0});
                for (std::size_t j = 0; j < n; ++j) {
                    if (j == i)
                        continue;
                    // Boxes farther apart than the farthest kept are no nearer.
                    const int gap = std::max({box[j][0] - box[i][2], box[i][0] - box[j][2], box[j][1] - box[i][3],
                                              box[i][1] - box[j][3], 0});
                    if (gap >= legs[policy::leg_neighbours - 1].first)
                        continue;
                    const int d = line_between(i, j);
                    for (std::size_t k = 0; k < policy::leg_neighbours; ++k)
                        if (d < legs[k].first) {
                            std::move_backward(legs.begin() + std::ptrdiff_t(k),
                                               legs.begin() + std::ptrdiff_t(policy::leg_neighbours - 1),
                                               legs.begin() + std::ptrdiff_t(policy::leg_neighbours));
                            legs[k] = {d, j};
                            break;
                        }
                }
                for (std::size_t k = n; k < ends; ++k) {
                    const int gap = std::max({box[k][0] - box[i][2], box[i][0] - box[k][2], box[k][1] - box[i][3],
                                              box[i][1] - box[k][3], 0});
                    if (gap >= legs.back().first)
                        continue;
                    const int d = line_between(i, k);
                    if (d < legs.back().first)
                        legs.back() = {d, k};
                }
                int best = 0;
                for (const auto &[line, to] : legs)
                    if (line != std::numeric_limits<int>::max())
                        best = std::max(best, price(i, to, line));
                plan.leg_saving[i / 2] |= std::uint8_t(std::min(15, best / 2) << (4 * (i % 2)));
            }
        }
        for (std::size_t i = 0; i < altars.size(); ++i)
            plan.altar_hop[i] = std::uint16_t(altar_hop_walk(altars[i], walk.get(), hop.get(), w, h));
        for (std::size_t i = 0; i < groups.size(); ++i) {
            int shape = 1;
            for (const auto &a : groups[i]) {
                int hits = 0;
                for (const auto &b : groups[i])
                    hits += distance(a.spawn_position(), b.spawn_position()) <=
                            bc::constants::passive_abilities::varg_splash_radius;
                shape = std::max(shape, hits);
            }
            plan.group_splash[i] = static_cast<std::uint8_t>(std::min(shape, 255));
        }
        plan.group_count = static_cast<std::uint16_t>(groups.size());
    }

    void write_roster(const compositions::Entry &entry) {
        // next_pick follows this same roster order.
        const auto bodies = compositions::expand(entry.picks);
        std::copy_n(bodies.types.begin(), plan.roster_types.size(), plan.roster_types.begin());
        plan.body_count = bodies.count;
    }
};
} // namespace

void play_draft() {
    static DraftState state;
    if (!state.ready)
        state.initialise();
    const auto chosen = bc::draft::current_draft(bc::team::ally);
    const auto committed = compositions::mask(chosen);
    const auto *selected = &compositions::roster_for(bc::draft::team_size());
    auto pick = compositions::next_pick(*selected, committed);
    if (!(selected->picks & compositions::bit(pick))) {
        // Roster exhausted (more picks than it holds): take any unpicked
        // champion but the twin pick, whose second body would outgrow the
        // per-body arrays (TeamMemory's slots, the plan's roster).
        for (unsigned p = 0; p < compositions::physical.size(); ++p)
            if (compositions::Pick(p) != compositions::Pick::cassi_and_issac &&
                !(committed & compositions::bit(compositions::Pick(p)))) {
                pick = compositions::Pick(p);
                break;
            }
    }
    state.write_roster(*selected);
    bc::draft_memory = std::span{&state.plan, 1};
    if constexpr (policy::diagnostic) {
        std::cout << "DRAFT " << bc::to_string(pick) << " roster=" << selected->name
                  << " bodies=" << unsigned(state.plan.body_count) << " fuel=" << bc::fuel_remaining()
                  << '\n';
    }
    bc::draft::pick(pick);
}

}  // namespace eng_a
