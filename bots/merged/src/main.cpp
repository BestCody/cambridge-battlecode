// Two engines in one bot: v119 (eng_a) and v130 (eng_b), chosen per map by
// its print (routes.h). Every body of the team computes the same print, so the
// draft and all champions agree.
#include "a/bot.h"
#include "a/draft/draft.h"
#include "b/bot.h"
#include "b/draft/draft.h"
#include "routes.h"
#include <cambc.h>
#include <iostream>
#include <vector>

namespace {
// Raw engine queries: cambc::map::camp_groups() also loads every camp of every
// group, about 34k fuel of the first turn.
char choose_engine() {
    using namespace cambc::detail;
    std::uint32_t n = 0;
    sdk_map_num_camp_groups(&n);
    std::vector<std::uint32_t> ids(n);
    sdk_map_camp_groups(ids.data(), n, &n);
    unsigned gems = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        camp_group_info info{};
        sdk_map_camp_group(ids[i], &info);
        gems += info.gems;
    }
    const int w = int(cambc::map::width()), h = int(cambc::map::height());
    char engine = default_engine;
    for (const auto &r : routes)
        if (r.width == w && r.height == h && r.groups == int(n) && r.gems == int(gems))
            engine = r.engine;
    return engine;
}

template <class Bot, class Draft>
[[noreturn]] void run(Draft draft) {
    Bot bot;
    while (true) {
        const auto start_round = cambc::round_number();
        const bool drafting = cambc::is_draft();
        try {
            if (drafting)
                draft();
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
}  // namespace

int main() {
    if (choose_engine() == 'a')
        run<eng_a::Bot>(eng_a::play_draft);
    else
        run<eng_b::Bot>(eng_b::play_draft);
}
