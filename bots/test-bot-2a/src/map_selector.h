#pragma once

#include <algorithm>
#include <array>
#include <cambc.h>
#include <cstdint>

namespace map_selector {

inline std::uint64_t fingerprint() {
    namespace bc = cambc;
    static const std::uint64_t value = [] {
        std::uint64_t h = 1469598103934665603ull;
        const auto mix = [&](std::uint64_t value) {
            for (unsigned byte = 0; byte < 8; ++byte) {
                h ^= (value >> (byte * 8)) & 0xffu;
                h *= 1099511628211ull;
            }
        };
        const auto board_hash = [](const bc::bitboard &board) {
            std::uint64_t value = 1469598103934665603ull;
            const auto positions = board.positions();
            for (const auto p : positions) {
                value ^= std::uint32_t(p.x);
                value *= 1099511628211ull;
                value ^= std::uint32_t(p.y);
                value *= 1099511628211ull;
            }
            value ^= positions.size();
            value *= 1099511628211ull;
            return value;
        };

        mix(std::uint32_t(bc::map::width()));
        mix(std::uint32_t(bc::map::height()));
        mix(std::uint32_t(bc::map::symmetry()));
        for (const auto *board :
             {&bc::map::walls(), &bc::map::bushes(), &bc::map::holes(), &bc::map::altars()})
            mix(board_hash(*board));

        std::array spawn_hashes{board_hash(bc::map::spawn_region(bc::team::ally)),
                                board_hash(bc::map::spawn_region(bc::team::enemy))};
        std::sort(spawn_hashes.begin(), spawn_hashes.end());
        for (const auto value : spawn_hashes)
            mix(value);

        const auto groups = bc::map::camp_groups();
        mix(groups.size());
        for (const auto &group : groups) {
            mix(group.gold_value());
            mix(group.gem_value());
            mix(std::uint32_t(group.max_respawn_timer()));
            mix(group.camps().size());
            for (const auto &camp : group.camps()) {
                const auto p = camp.spawn_position();
                mix(std::uint32_t(camp.kind()));
                mix(std::uint32_t(p.x));
                mix(std::uint32_t(p.y));
            }
        }
        return h;
    }();
    return value;
}

bool use_specialized(std::uint64_t fingerprint);

} // namespace map_selector
