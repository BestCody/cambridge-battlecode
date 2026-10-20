#pragma once
#include "../strategy.h"

enum class SquadRole { attack, gather };
enum class SquadPhase { select, travel, clear, capture, deposit, recover };
enum class BankStage : unsigned {
    travel,
    ready,
    crossing,
    crossed,
    assembled,
    guard,
    channel,
    done,
    preparing,
    prepared,
    count
};
static_assert(unsigned(BankStage::count) == TeamMemory::bank_stages);

enum class MoveStatus { moved, arrived, waiting, blocked, deferred };
enum class MoveReason { none, allowance, occupancy, support, terrain, camp, fuel };
struct MoveResult {
    MoveStatus status = MoveStatus::arrived;
    MoveReason reason = MoveReason::none;
    int blocker = -1, until = -1;
    explicit operator bool() const { return status == MoveStatus::moved; }
};
enum class Selection { selected, deferred, unavailable };

struct FiringPosition {
    bool evaluated = false;
    vec2 tile{-1, -1}, capture{-1, -1};
    int arrival = 0, ready = 0, occupancy = 0;
    float output = 0, exposure = 0;
};

struct JobEstimate {
    int camp = -1, travel = 0, reward_time = 0, delivery = 0, assessed = -1;
    // Earliest round offset at which any enemy, seen or hidden, could reach
    // the capture region. Larger than reward_time means an uncontested clear.
    int interference = bc::distance_field::unreachable;
    float rate = 0, loss = 0, clear = 0, output = 0;
    bool pressure = false, siege = false, complete = false, control = true;
    unsigned participants = 0;
    // Material state at assessment. A change invalidates the plan; age alone
    // only expires it after policy::plan_lifetime_windows.
    unsigned revision = ~0u;
    int status = -1, threats = 0;
    vec2 staging{-1, -1};
    std::array<FiringPosition, TeamMemory::slots> firing;
};

struct FiringSearch {
    unsigned stage = 0, cursor = 0;
    struct Candidate {
        vec2 tile;
        int path, range;
        float cost;
    };
    std::vector<vec2> tiles;
    std::vector<Candidate> ranked;
    FiringPosition best;
    float value = -1;
};

struct ObjectiveSearch {
    int camp = -1, started = -1;
    unsigned next_member = 0, membership = 0, stage = 0, capture_member = 0;
    int clear_reward_time = 0, occupancy = 0;
    unsigned next_enemy = 0;
    std::vector<unsigned> control_enemies;
    std::optional<bc::distance_field> capture_routes;
    std::array<int, TeamMemory::slots> next_action{};
    std::array<std::array<int, 2>, TeamMemory::slots> cast_ready{};
    float interference_work = 0, exchange_loss = 0;
    JobEstimate result;
    FiringSearch firing;
};

struct ClearProgress {
    int camp = -1, last = -1, rounds = 0;
    float damage = 0, expected = 0, efficiency = 1;
};

struct OriginRoute {
    vec2 origin{-1, -1};
    unsigned revision = ~0u;
    bc::distance_field field;
};

struct TravelRoute {
    unsigned revision = ~0u;
    bc::bitboard goals;
    bc::distance_field field;
};

// Local reconstruction of the shared job, never a second copy of live stats.
struct SquadState {
    unsigned members = 0;
    int coordinator = -1, camp = -1, altar = -1, resume_camp = -1;
    SquadRole role = SquadRole::attack;
    SquadPhase phase = SquadPhase::select;
    BankStage bank_stage = BankStage::travel;
    int changed = 0, progress = 0, review = -1, reviewed_gems = -1, last_enemies = -1;
    int failed_camp = -1, retry_after = 0;
    int next_job_review = 0, route_best = bc::distance_field::unreachable;
    int motion_since = 0, failed_altar = -1, altar_retry = 0;
    MoveResult motion;
    vec2 last_step{-1, -1};
    unsigned participants = 0, enemy_score = 0;
    int score_round = 0;
    float enemy_pace = 0;
    std::array<unsigned, TeamMemory::slots> requests{};
    std::array<int, TeamMemory::slots> request_seen{};
    float pressure_rate = 1, batch = 0;
    bool need_crystal = false, access_preparation = false, upgrade_preparation = false, siege = false;
    std::array<vec2, TeamMemory::slots> last_positions{};
    std::array<int, TeamMemory::slots> last_seen{-1, -1, -1, -1, -1, -1};
    bool contains(unsigned slot) const { return members & (1u << slot); }
    JobEstimate plan;
    int arrival_deadline = -1, crossing_deadline = -1;
    unsigned crossed = 0;
    std::array<int, TeamMemory::slots> cargo_since{-1, -1, -1, -1, -1, -1};
    std::array<unsigned, TeamMemory::slots> cargo_seen{};
    int cargo_deadline = -1;
    // Material events observed by the coordinator. Each one forces a review of
    // the current job instead of waiting for the next scheduled window.
    unsigned last_alive = 0, capture_resets = 0;
    int last_threats = -1;
    bool was_holding = false;
};

struct JobSearch {
    std::vector<unsigned> order;
    std::vector<std::pair<std::pair<bool, float>, unsigned>> ranked;
    unsigned rank_cursor = 0;
    std::vector<JobEstimate> assessed;
    unsigned cursor = 0;
    int started = -1;
    ObjectiveSearch work;
};

// Cached terrain access: walking first, then one crystal across a barrier.
struct AltarAccess {
    bool prepared = false;
    bc::bitboard region, entries;
    bc::distance_field entry_routes;
    vec2 rendezvous{-1, -1};
    std::vector<vec2> entry_candidates;
    unsigned prepare_stage = 0, entry_cursor = 0;
    int entry_distance = bc::distance_field::unreachable;
    std::vector<vec2> landings;
};

struct BankEstimate {
    int altar = -1, travel = 0, access = 0, channel = 0, exit = 0, locks = 0;
    int interference = bc::distance_field::unreachable;
    unsigned cargo = 0;
    float cost = 0, overhead = 0;
    bool teleport = false, supply = false, recall_exit = false, winning = false;
    bool feasible = false;
    vec2 entry{-1, -1};
    unsigned participants = 0, guards = 0;
    std::array<vec2, TeamMemory::slots> positions{};
    std::array<int, TeamMemory::slots> arrivals{}, starts{}, finishes{};
    int completion = 0;
};

enum class ActionResult { blocked, progress, complete };

struct FiringOpportunity {
    float damage = 0, control = 0;
};
