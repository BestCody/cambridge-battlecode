// Dispatch draft and gameplay; a failed turn is logged and still yields.
#include "bot.h"
#include "draft/draft.h"
#include <cambc.h>
#include <iostream>

int main() {
    Bot bot;
    while (true) {
        const auto start_round = cambc::round_number();
        const bool drafting = cambc::is_draft();
        try {
            if (cambc::is_draft())
                play_draft();
            else
                bot.play_turn();
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
