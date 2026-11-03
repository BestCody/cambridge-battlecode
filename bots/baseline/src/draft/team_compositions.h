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
// One roster per pick count. The twin pick (Cassi and Issac, two bodies for
// one pick) is in every top team's draft on 2026-11-03 (Mite, Intercontinental
// Fish, Ad Astra, newer team), and in their games Cassi and Issac clear as
// many camps each as Vesper or Heart of Ruin (228 and 220 clears to 191 and
// 238 over 33 games). At 5 picks it replaces Ryth; at 4, Vendetta; at 3,
// Marcella.
inline constexpr Mask core_picks = roster(Pick::heart_of_ruin, Pick::marcella, Pick::vesper_vale);
// v122 drafted the twin everywhere (the three twin rosters below, kept): on
// two wide practice blocks against v119 in the same hours it scored 24/50
// against 27/50 and 17/35 against 24/35 (Cassi and Issac, 600 and 900
// health, dead together three to five times a game in team fights; the
// games it took were Ding's vault and membrane and Nanyang's quarry and
// reef). The five-body rosters stand until the twin can be kept alive.
inline constexpr bool twin_rosters = false;
inline constexpr std::array twin_entries{
    Entry{"HeartOfRuin/Vesper/Cassi&Issac", roster(Pick::heart_of_ruin, Pick::vesper_vale, Pick::cassi_and_issac)},
    Entry{"HeartOfRuin/Marcella/Vesper/Cassi&Issac", Mask(core_picks | bit(Pick::cassi_and_issac))},
    Entry{"HeartOfRuin/Marcella/Vesper/Vendetta/Cassi&Issac",
          Mask(core_picks | roster(Pick::vendetta, Pick::cassi_and_issac))}};
inline constexpr std::array five_entries{
    Entry{"HeartOfRuin/Marcella/Vesper", core_picks},
    Entry{"HeartOfRuin/Marcella/Vesper/Vendetta", Mask(core_picks | bit(Pick::vendetta))},
    Entry{"HeartOfRuin/Marcella/Vesper/Vendetta/Ryth", Mask(core_picks | roster(Pick::vendetta, Pick::ryth))}};
inline constexpr std::array rosters = twin_rosters ? twin_entries : five_entries;
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
