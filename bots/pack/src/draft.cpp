// The fixed draft: melee farmers with a dash for pockets, one ranged carry.
#include <algorithm>
#include <array>
#include <cambc.h>

namespace bc = cambc;
using Pick = bc::champion_draft;

void play_draft() {
    const auto chosen = bc::draft::current_draft(bc::team::ally);
    const unsigned size = bc::draft::team_size();
    // Rosters by team size. Heart of Ruin and Vendetta clear fast and cross
    // holes; Vesper banks 33% more gold; Marcella bursts; Varg shields.
    static constexpr std::array five{Pick::vendetta, Pick::heart_of_ruin, Pick::vesper_vale, Pick::marcella,
                                     Pick::varg};
    const unsigned count = std::min<unsigned>(size, five.size());
    for (unsigned i = 0; i < count; ++i) {
        const Pick pick = five[i];
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            bc::draft::pick(pick);
            return;
        }
    }
    // Every planned pick is taken: anything released.
    for (int i = 0; i < 12; ++i) {
        const auto pick = static_cast<Pick>(i);
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            bc::draft::pick(pick);
            return;
        }
    }
}
