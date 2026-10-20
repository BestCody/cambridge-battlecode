// Draft: pick the fixed roster for this team size and write the opening plan
// to draft memory.
#include "draft.h"
#include "../strategy.h"
#include "team_compositions.h"
#include <iostream>

namespace {
struct DraftState {
    bool ready = false;
    DraftPlan plan;

    void initialise() {
        const auto groups = bc::map::camp_groups();
        auto walls = bc::map::walls() | bc::map::holes() | bc::map::altars();
        float dangerous = 0, total_splash = 0;
        unsigned productive = 0;
        unsigned cheap_gem_groups = 0;
        for (const auto &g : groups) {
            float h = 0, p = 0, area = 1, toughest = 1;
            bool farmable = true;
            for (const auto &c : g) {
                auto pos = c.spawn_position();
                auto stats = c.base_stats();
                h += stats.base_health;
                p += stats.base_strength;
                // Classified per camp, not per group: three elks are a 7,200
                // health group but still farmable, whereas one monolith in a
                // group defends the whole group.
                if (stats.base_health > policy::cheap_camp_health ||
                    stats.base_strength > policy::cheap_camp_strength)
                    farmable = false;
                toughest = std::max(toughest, stats.base_health);
                float hits = 0;
                for (const auto &other : g)
                    if (distance(pos, other.spawn_position()) <=
                        bc::constants::passive_abilities::varg_splash_radius)
                        ++hits;
                area = std::max(area, hits);
            }
            area = std::min(area, h / toughest);
            if (p > 0)
                ++dangerous;
            if (g.gem_value() > 0) {
                ++productive;
                if (farmable)
                    ++cheap_gem_groups;
            }
            total_splash += area;
        }
        plan.cheap_gem_groups = static_cast<std::uint8_t>(std::min(cheap_gem_groups, 255u));
        if (!groups.empty()) {
            plan.mean_splash = total_splash / groups.size();
            plan.dangerous_fraction = dangerous / groups.size();
        }
        plan.spacing = std::sqrt(float((~walls).count()) / std::max(1u, productive));
        ready = true;
    }

    void write_roster(const compositions::Entry &entry) {
        // next_pick follows this same roster order, including both Twin bodies.
        const auto bodies = compositions::expand(entry.picks);
        plan.roster_types = bodies.types;
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
        // Roster exhausted (more picks than it holds): take any unpicked champion.
        for (unsigned p = 0; p < compositions::physical.size(); ++p)
            if (!(committed & compositions::bit(compositions::Pick(p)))) {
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
