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
// One roster per pick count, copied from the top ladder team's lineup since
// engine 1.0.3: Vesper, Vendetta and Heart of Ruin carry, Twins (Cassi and
// Issac) add two farming bodies at 4 picks and Ryth a melee fighter at 5.
// Vesper and Vendetta collect most of its gems and gold. It won 65 of 80 head
// to head against its earlier lineup (Stellar, Heart of Ruin, Ren) and 194 vs
// 168 of 210 against the old bots (deaths 12.3 vs 19.4 a game).
inline constexpr Mask core_picks = roster(Pick::vesper_vale, Pick::vendetta, Pick::heart_of_ruin);
inline constexpr std::array rosters{
    Entry{"Vesper/Vendetta/HeartOfRuin", core_picks},
    Entry{"Vesper/Vendetta/HeartOfRuin/Twins", Mask(core_picks | bit(Pick::cassi_and_issac))},
    Entry{"Vesper/Vendetta/HeartOfRuin/Twins/Ryth",
          Mask(core_picks | roster(Pick::cassi_and_issac, Pick::ryth))}};
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
