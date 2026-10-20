// Per-turn dispatch between the squad and worker controllers.
#include "bot.h"

void Bot::play_turn() {
    round = bc::round_number();
    walked = false;
    if (!initialised) {
        initialise();
        if (bc::fuel_remaining() < policy::observation_reserve + policy::planning_reserve) {
            publish(TeamMemory::unavailable);
            return;
        }
    }
    if (squad_member()) {
        play_squad_turn();
        squad_priority();
        prepare_routes();
        return;
    }
    play_worker_turn();
    worker_priority();
    prepare_routes();
}
