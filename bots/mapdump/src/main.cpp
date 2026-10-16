// mapdump — offline tool: print every camp group in a map during the draft phase.
//
// Built and run like a bot:  cambc build mapdump ; cambc run mapdump mapdump --map contest
// The output lands in <outdir>/team-a.stdout.
//
// Purpose: audit which maps actually contain gem-bearing camps, which is the
// input the farming planner needs and cannot infer from the map name.
#include <cambc.h>
#include <iostream>
#include <map>
#include <string>

int main() {
    while (true) {
        if (cambc::is_draft()) {
            static bool dumped = false;
            if (!dumped) {
                dumped = true;
                const auto &walls = cambc::map::walls();
                const auto &holes = cambc::map::holes();
                const auto &altars = cambc::map::altars();
                auto groups = cambc::map::camp_groups();
                std::cout << "MAP " << cambc::map::width() << "x" << cambc::map::height()
                          << " walls=" << walls.count() << " holes=" << holes.count()
                          << " altars=" << altars.count() << " groups=" << groups.size() << '\n';
                std::map<std::string, int> tally;
                int index = 0;
                for (const auto &group : groups) {
                    std::cout << "GROUP " << index++ << " gems=" << group.gem_value()
                              << " gold=" << group.gold_value()
                              << " respawn=" << group.max_respawn_timer() << " members=";
                    for (const auto &c : group) {
                        const auto stats = c.base_stats();
                        const auto pos = c.spawn_position();
                        const std::string name = cambc::to_string(c.kind());
                        ++tally[name];
                        std::cout << name << '@' << pos.x << ',' << pos.y << "(hp="
                                  << static_cast<int>(stats.base_health)
                                  << ",str=" << static_cast<int>(stats.base_strength) << ") ";
                    }
                    std::cout << '\n';
                }
                std::cout << "TALLY";
                for (const auto &[name, count] : tally)
                    std::cout << ' ' << name << '=' << count;
                std::cout << std::endl;
            }
            cambc::draft::pick(cambc::champion_draft::varg);
        }
        cambc::end_turn();
    }
}
