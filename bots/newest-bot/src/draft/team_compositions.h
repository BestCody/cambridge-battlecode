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
// One roster per pick count. Vesper, Vendetta and Heart of Ruin carry (from
// the top ladder team's lineup since engine 1.0.3: 65 of 80 head to head
// against the earlier Stellar, Heart of Ruin, Ren). Ryth joins at 4 picks and
// Marcella, which most ladder teams draft, at 5. The Twins they replace died
// together, both bodies to one kill: 43% of our ladder deaths. Without them
// the team died 14-16 times a game instead of 24-26 and won 135 of 240 head
// to head (level with the old bots and the version before the lineup).
inline constexpr Mask core_picks = roster(Pick::vesper_vale, Pick::vendetta, Pick::heart_of_ruin);
inline constexpr std::array rosters{
    Entry{"Vesper/Vendetta/HeartOfRuin", core_picks},
    Entry{"Vesper/Vendetta/HeartOfRuin/Ryth", Mask(core_picks | bit(Pick::ryth))},
    Entry{"Vesper/Vendetta/HeartOfRuin/Ryth/Marcella",
          Mask(core_picks | roster(Pick::ryth, Pick::marcella))}};
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
