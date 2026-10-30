// Draft: pick the fixed roster for this team size and write the opening plan
// to draft memory.
#include "draft.h"
#include "../strategy.h"
#include "team_compositions.h"
#include <iostream>

namespace {
// Shared memory has a slot per body: every roster fits, and none has the twin
// pick's second body.
constexpr bool rosters_fit() {
    for (const auto &entry : compositions::rosters)
        if (entry.size() > TeamMemory::slots ||
            (entry.picks & compositions::bit(compositions::Pick::cassi_and_issac)))
            return false;
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
        for (const auto &g : groups) {
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
        plan.spacing = std::sqrt(float((~walls).count()) / std::max(1u, productive));
        measure_groups(groups);
        ready = true;
    }

    void measure_groups(const std::vector<bc::map::camp_group> &groups) {
        // The same searches a champion's initialisation would run (world.cpp).
        if (groups.size() > DraftPlan::max_groups)
            return;
        const auto static_blocked = terrain();
        bc::distance_field home, enemy, altar;
        bc::bitboard visited;
        home.compute(static_blocked, bc::map::spawn_region(bc::team::ally), visited);
        enemy.compute(bc::map::walls() | bc::map::holes() | bc::map::altars(),
                      bc::map::spawn_region(bc::team::enemy), visited);
        altar.compute(static_blocked, altar_goals_of(bc::map::altars().positions(), static_blocked), visited);
        for (std::size_t i = 0; i < groups.size(); ++i) {
            auto &distance = plan.group_distance[i];
            distance.fill(bc::distance_field::unreachable);
            for (const auto &c : groups[i]) {
                const auto p = c.spawn_position();
                distance[DraftPlan::home] = std::min(distance[DraftPlan::home], home.get(p));
                distance[DraftPlan::enemy] = std::min(distance[DraftPlan::enemy], enemy.get(p));
                distance[DraftPlan::altar] = std::min(distance[DraftPlan::altar], altar.get(p));
            }
        }
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
