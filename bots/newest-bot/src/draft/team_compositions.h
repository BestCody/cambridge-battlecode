#pragma once
#include <array>
#include <bit>
#include <cambc.h>
#include <cstdint>
#include <span>

// One fixed roster per supported pick count. Archived alternatives are in docs/design-notes.md.
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
// Every roster supplies the Stellar/Tavi/Cassi squad; Twins also supplies Issac.
inline constexpr Mask core_picks = roster(Pick::stellar, Pick::tavi, Pick::cassi_and_issac);
inline constexpr std::array rosters{
    Entry{"Stellar/Tavi/Twins", core_picks},
    Entry{"Ryth/Stellar/Tavi/Twins", Mask(core_picks | bit(Pick::ryth))},
    Entry{"Varg/Ryth/Stellar/Tavi/Twins", Mask(core_picks | roster(Pick::varg, Pick::ryth))}};
static_assert(rosters.size() == 3 && rosters[0].size() == 3 && rosters[1].size() == 4 &&
              rosters[2].size() == 5);

constexpr unsigned fixed_roster(unsigned count) {
    for (unsigned i = 0; i < rosters.size(); ++i)
        if (rosters[i].size() == count)
            return i;
    return rosters.size();
}

inline constexpr std::array physical{Type::ryth,    Type::vesper_vale, Type::vendetta, Type::cassi,
                                     Type::stellar, Type::prism,       Type::marcella, Type::willow,
                                     Type::ren,     Type::varg,        Type::tavi};
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
// Stable pick order keeps roster and turn-order changes out of squad
// experiments. The caller only requests a pick while the fixed roster is
// incomplete.
inline Pick next_pick(unsigned selected, Mask chosen) {
    const Mask remaining = rosters[selected].picks & ~chosen;
    return Pick(std::countr_zero(remaining));
}
} // namespace compositions
