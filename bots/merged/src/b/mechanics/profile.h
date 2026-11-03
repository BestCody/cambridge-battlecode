#pragma once
namespace eng_b {
// Per-stage fuel accounting for local test builds (policy::profile). Compiled
// out otherwise. Stages nest, so a stage's fuel includes its callees.
}  // namespace eng_b
#include "../strategy.h"
namespace eng_b {

namespace profiling {
enum class Stage {
    turn,
    buy,
    observe,
    retreat,
    choose,
    skirmish,
    fight_search,
    tribute,
    formation,
    reinforce,
    farm,
    move,
    combat,
    ward,
    priority,
    background,
    routes,
    sieges,
    diagnostics,
    count
};
inline constexpr const char *stage_names[] = {
    "turn",    "buy",       "observe",   "retreat",  "choose",     "skirmish", "search",
    "tribute", "formation", "reinforce", "farm",     "move",       "combat",   "ward",
    "priority", "background", "routes",  "sieges",   "diag"};
static_assert(std::size(stage_names) == static_cast<std::size_t>(Stage::count));

struct Totals {
    unsigned long long fuel = 0, calls = 0, peak = 0;
};
inline Totals totals[static_cast<int>(Stage::count)];
// Turns whose total fuel exceeded 50k, 70k and 90k.
inline unsigned long long heavy_turns[3];
// This turn's fuel by stage, printed for any turn past 85k.
inline unsigned long long turn_fuel[static_cast<int>(Stage::count)];

inline void report_heavy_turn(unsigned long long used) {
    if (used > 85000) {
        std::printf("HEAVY r=%d used=%llu", int(bc::round_number()), used);
        for (int i = 1; i < static_cast<int>(Stage::count); ++i)
            if (turn_fuel[i])
                std::printf(" %s=%llu", stage_names[i], turn_fuel[i]);
        std::printf("\n");
    }
    for (auto &fuel : turn_fuel)
        fuel = 0;
}

class Scope {
    Stage stage;
    std::uint64_t start;

  public:
    explicit Scope(Stage stage) : stage(stage), start(policy::profile ? bc::fuel_remaining() : 0) {}
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    ~Scope() {
        if constexpr (policy::profile) {
            const auto now = bc::fuel_remaining();
            const auto used = start > now ? start - now : 0;
            auto &t = totals[static_cast<int>(stage)];
            t.fuel += used;
            ++t.calls;
            t.peak = std::max<unsigned long long>(t.peak, used);
            turn_fuel[static_cast<int>(stage)] += used;
            if (stage == Stage::turn) {
                for (int i = 0; i < 3; ++i)
                    heavy_turns[i] += used > std::uint64_t(50000 + 20000 * i);
                report_heavy_turn(used);
            }
        }
    }
};

inline void report(unsigned round, int type) {
    if constexpr (!policy::profile)
        return;
    std::printf("PROFILE r=%u type=%d heavy=%llu/%llu/%llu", round, type, heavy_turns[0], heavy_turns[1],
                heavy_turns[2]);
    for (int i = 0; i < static_cast<int>(Stage::count); ++i)
        std::printf(" %s=%llu/%llu/%llu", stage_names[i], totals[i].fuel, totals[i].calls, totals[i].peak);
    std::printf("\n");
}
} // namespace profiling

}  // namespace eng_b
