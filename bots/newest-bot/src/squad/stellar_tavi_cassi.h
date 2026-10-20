#pragma once
#include "../strategy.h"

namespace squads {
// One composition policy for all three fixed roster sizes.
struct StellarTaviCassi {
    static unsigned membership(std::span<const bc::champion_type> roster);
};
} // namespace squads
