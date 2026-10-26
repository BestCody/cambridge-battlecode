#pragma once
#include <array>
#include <bit>
#include <cambc.h>
#include <cstdint>
#include <span>

// The fixed draft. Archived alternatives are in docs/design-notes.md.
namespace compositions {
using Pick = cambc::champion_draft;
using Type = cambc::champion_type;
using Mask = std::uint16_t;
constexpr Mask bit(Pick pick) { return Mask(1u << unsigned(pick)); }
template <class... Picks>
constexpr Mask roster(Picks... picks) {
    return (bit(picks) | ...);
}
struct Entry {
    const char *name;
    Mask picks;
    constexpr unsigned size() const { return std::popcount(picks); }
};
// One roster per pick count: willow best girl's, the same in all 40 of their
// ranked games we studied (2026-10-24): Heart of Ruin, Prism and Vesper at 3
// picks, Marcella joins at 4 and Willow at 5.
inline constexpr Mask core_picks = roster(Pick::heart_of_ruin, Pick::prism, Pick::vesper_vale);
inline constexpr std::array rosters{
    Entry{"HeartOfRuin/Prism/Vesper", core_picks},
    Entry{"HeartOfRuin/Prism/Vesper/Marcella", Mask(core_picks | bit(Pick::marcella))},
    Entry{"HeartOfRuin/Prism/Vesper/Marcella/Willow", Mask(core_picks | roster(Pick::marcella, Pick::willow))}};
static_assert(rosters[0].size() == 3 && rosters[1].size() == 4 && rosters[2].size() == 5);

// The roster for this pick count, or the largest one for an unsupported count.
constexpr const Entry &roster_for(unsigned count) {
    for (const auto &entry : rosters)
        if (entry.size() == count)
            return entry;
    return rosters.back();
}

inline constexpr std::array physical{Type::ryth,    Type::vesper_vale, Type::vendetta, Type::cassi,
                                     Type::stellar, Type::prism,       Type::marcella, Type::willow,
                                     Type::ren,     Type::varg,        Type::tavi,     Type::heart_of_ruin};
inline Mask mask(std::span<const Pick> picks) {
    Mask result = 0;
    for (auto pick : picks)
        result |= bit(pick);
    return result;
}
struct Bodies {
    std::array<Type, 6> types{};
    unsigned count = 0;
    void append(Pick pick) {
        types[count++] = physical[unsigned(pick)];
        if (pick == Pick::cassi_and_issac)
            types[count++] = Type::issac;
    }
};
inline Bodies expand(Mask picks) {
    Bodies result;
    for (unsigned p = 0; p < physical.size(); ++p)
        if (picks & bit(Pick(p)))
            result.append(Pick(p));
    return result;
}
// Picks go in ascending enum order, which is also the order of the bodies'
// handles that gameplay relies on.
inline Pick next_pick(const Entry &selected, Mask chosen) {
    const Mask remaining = selected.picks & ~chosen;
    return Pick(std::countr_zero(remaining));
}
} // namespace compositions
