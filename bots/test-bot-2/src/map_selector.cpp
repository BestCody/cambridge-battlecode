#include "map_selector.h"

namespace map_selector {

bool use_specialized(std::uint64_t value) {
    switch (value) {
    case 6055844582290853923ull:  // 00collision
    case 4910504328211245912ull:  // 00dunes
    case 14023506871427809951ull: // 00elkring
    case 920872663342396652ull:   // 00monolith
    case 147049049298006171ull:   // 00quadrants
    case 17312433488281463600ull: // 00ribs
    case 4319751591402114054ull:  // 00rushbait-v2
    case 3002013053572930898ull:  // 00the-line
    case 6435952725725139703ull:  // 00vault
    case 5201011070176381059ull:  // 01big-world
    case 4050668476935346536ull:  // 01boss-battle
    case 557300913417793298ull:   // 02harvest
    case 5416573089676087884ull:  // 02stalactites
    case 9732021182415588261ull:  // adgato-gemcombat1
    case 6676767198827392790ull:  // adgato-gemcombat2
    case 2293371628172449702ull:  // adgato-gemcombat3
    case 13528246662461452064ull: // adgato-gemcombat5
        return true;
    default:
        return false;
    }
}

} // namespace map_selector
