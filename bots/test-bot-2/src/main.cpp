// Dispatch draft and gameplay; a failed turn is logged and still yields.
#include "bot.h"
#include "draft/draft.h"
#include "fallback/fallback_api.h"
#include "map_selector.h"
#include <cambc.h>
#include <iostream>
#include <memory>

int main() {
    const bool specialized = map_selector::use_specialized(map_selector::fingerprint());
    auto bot = specialized ? std::make_unique<Bot>() : nullptr;
    auto fallback_bot = specialized ? nullptr : std::make_unique<fallback::Bot>();
    while (true) {
        const auto start_round = cambc::round_number();
        const bool drafting = cambc::is_draft();
        try {
            if (drafting) {
                if (specialized)
                    play_draft();
                else
                    fallback::play_draft();
            } else if (specialized)
                bot->play_turn();
            else
                fallback_bot->play_turn();
        } catch (const std::exception &error) {
            std::cout << "bot error: " << error.what() << std::endl;
        } catch (...) {
            std::cout << "bot error: unknown exception" << std::endl;
        }

        if (!drafting && cambc::round_number() != start_round)
            std::cout << "FUEL_OVERRUN started=" << start_round << " ended=" << cambc::round_number()
                      << " type=" << int(cambc::self().champion_name()) << '\n';
        cambc::end_turn();
    }
}
