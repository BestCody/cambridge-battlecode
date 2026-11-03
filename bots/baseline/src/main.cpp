// Two engines in one bot: v119 (eng_a) plays from the start; if the team is
// behind in banked gems from mid-game on, main's baseline (eng_b) takes over.
#include "a/bot.h"
#include "a/draft/draft.h"
#include "b/bot.h"
#include "b/draft/draft.h"
#include <cambc.h>
#include <iostream>

namespace {
constexpr int switch_from = 400;
constexpr int switch_gap = 100;

bool behind() {
    const int ours = int(cambc::global_submitted_gems(cambc::team::ally));
    const int theirs = int(cambc::global_submitted_gems(cambc::team::enemy));
    return theirs - ours >= switch_gap;
}
}  // namespace

int main() {
    eng_a::Bot first;
    eng_b::Bot *other = nullptr;
    while (true) {
        const auto start_round = cambc::round_number();
        const bool drafting = cambc::is_draft();
        // An action refused on a turn that ran out of fuel and resumed: what it
        // acted on has moved. The new round is played from a fresh look.
        const auto replay = [&] {
            if (drafting || cambc::round_number() == start_round)
                return;
            try {
                if (other)
                    other->play_turn();
                else
                    first.play_turn();
            } catch (...) {
            }
        };
        try {
            if (drafting)
                eng_a::play_draft();
            else {
                if (!other && int(start_round) >= switch_from && behind())
                    other = new eng_b::Bot();
                if (other)
                    other->play_turn();
                else
                    first.play_turn();
            }
        } catch (const std::exception &error) {
            std::cout << "bot error: " << error.what() << std::endl;
            replay();
        } catch (...) {
            std::cout << "bot error: unknown exception" << std::endl;
            replay();
        }
        if (!drafting && cambc::round_number() != start_round)
            std::cout << "FUEL_OVERRUN started=" << start_round << " ended=" << cambc::round_number()
                      << " type=" << int(cambc::self().champion_name()) << std::endl;
        // Debug builds' prints go out here, at no cost to the turn.
        debug_block({ std::fflush(nullptr); });
        cambc::detail::sdk_yield();
    }
}
