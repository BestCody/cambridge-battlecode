#pragma once
#include <array>
#include <bit>
#include <cambc.h>
#include <cstdint>
#include <span>

// Two rosters per supported pick count: offensive (with the squad) or resource
// (all farmers). The draft heuristic chooses. Archived alternatives are in
// docs/design-notes.md.
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
// Offensive rosters supply the Stellar/Tavi/Cassi squad (Twins also supplies
// Issac); extra picks become workers.
inline constexpr Mask core_picks = roster(Pick::stellar, Pick::tavi, Pick::cassi_and_issac);
inline constexpr std::array offensive{
    Entry{"Stellar/Tavi/Twins", core_picks},
    Entry{"Ryth/Stellar/Tavi/Twins", Mask(core_picks | bit(Pick::ryth))},
    Entry{"Varg/Ryth/Stellar/Tavi/Twins", Mask(core_picks | roster(Pick::varg, Pick::ryth))}};
// Resource rosters farm with every body: without Stellar and Tavi no squad
// forms. Base is the archived "spread-out gathering and rapid camp clearing"
// team; Twins is one pick for two farmers.
inline constexpr Mask gather_picks = roster(Pick::varg, Pick::vendetta, Pick::cassi_and_issac);
inline constexpr std::array resource{
    Entry{"Varg/Vendetta/Twins", gather_picks},
    Entry{"Varg/Vendetta/Ryth/Twins", Mask(gather_picks | bit(Pick::ryth))},
    Entry{"Varg/Vendetta/Ryth/Vesper/Twins", Mask(gather_picks | roster(Pick::ryth, Pick::vesper_vale))}};
static_assert(offensive[0].size() == 3 && offensive[1].size() == 4 && offensive[2].size() == 5);
static_assert(resource[0].size() == 3 && resource[1].size() == 4 && resource[2].size() == 5);

// The roster for this pick count, or nullptr for an unsupported count.
constexpr const Entry *roster_for(unsigned count, bool offense) {
    const auto &table = offense ? offensive : resource;
    for (const auto &entry : table)
        if (entry.size() == count)
            return &entry;
    return nullptr;
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
inline Pick next_pick(const Entry &selected, Mask chosen) {
    const Mask remaining = selected.picks & ~chosen;
    return Pick(std::countr_zero(remaining));
}
} // namespace compositions
