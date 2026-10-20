#pragma once
#include "squad/squad.h"
#include "squad/stellar_tavi_cassi.h"
#include "strategy.h"
#include <variant>

class Bot;

struct CampMemory {
    bc::camp unit;
    vec2 position;
    bc::camp_stats stats;
    float health;
    static bc::camp_stats design(const bc::camp &c) {
        static std::array<std::optional<bc::camp_stats>, static_cast<unsigned>(bc::camp_type::monolith) + 1>
            cache;
        auto &entry = cache[static_cast<unsigned>(c.kind())];
        if (!entry)
            entry = c.base_stats();
        return *entry;
    }
    explicit CampMemory(bc::camp c)
        : unit(c), position(c.spawn_position()), stats(design(c)), health(stats.base_health) {}
};
struct CampGroup {
    bc::map::camp_group unit;
    std::vector<CampMemory> camps;
    bc::bitboard tiles;
    vec2 centre{}, approach{-1, -1};
    bool approach_known = false;
    int obstacle_status = -1;
    float obstacle_health = -1;
    float max_health = 0, health = 0, pressure = 0, max_pressure = 0, splash = 1, shape_splash = 1,
          full_splash = 1;
    int seen = -1, respawn = 0, attacked = -10000;
    int holding_since = -1;
    int home_distance = 0, enemy_distance = 0, altar_distance = 0;
    enum Status { alive, capture, cooling } status = alive;
    explicit CampGroup(bc::map::camp_group g);
};
struct ObservedUnit {
    std::variant<bc::champion, bc::camp> ref;
    vec2 position;
    float health = 0, max_health = 0, strength = 0, focus = 0, shield = 0, mitigation = 1;
    float damage_rate = 0, burst = 0, ready_shield = 0, healing_rate = 0;
    float camp_multiplier = 1, camp_mitigation = 1;
    int move_speed = 1;
    int spell_reach = 0;
    int range = 0, group = -1, gems = 0, stun_time = -1;
    bool stunned = false, channel = false, depositing = false, recalling = false, immune = false;
    bool completed = false, can_attack = false, can_cast = false, can_move = false;
    bool movement_spent = false;
    bc::bitboard sight;
    std::array<Spell, 2> abilities;
    std::array<int, 2> cooldowns{};
    bc::champion_type type{};
    explicit ObservedUnit(bc::champion c);
    explicit ObservedUnit(const Bot &bot);
    ObservedUnit(const CampMemory &c, int group_index);
    const bc::unit &unit() const {
        return std::visit([](const auto &x) -> const bc::unit & { return x; }, ref);
    }
    bool enemy() const { return std::holds_alternative<bc::champion>(ref); }
    float retaliation() const {
        return enemy() && type == bc::champion_type::ren
                   ? focus * bc::constants::passive_abilities::ren_retaliation_focus_ratio
                   : 0;
    }
};

class Bot {
    friend struct ObservedUnit;
    bool initialised = false, walked = false, retreating = false, banking = false;
    unsigned slot = 0, round = 0;
    int target = -1, last_plan = -10000, last_report = -1000, stalls = 0;
    vec2 previous{-1, -1}, position{}, ward{-1, -1}, origin_position{-1, -1};
    float health = 0, max_health = 0, strength = 0, focus = 0, camp_multiplier = 1, camp_mitigation = 1,
          mitigation = 1;
    float nearby_healing = 0;
    unsigned gems = 0;
    int range = 1;
    bc::champion_type type{};
    bc::passive_type passive{};
    bc::champion_stats stats{};
    std::array<Spell, 2> spells;
    std::vector<CampGroup> groups;
    std::vector<bc::champion> roster;
    std::vector<ObservedUnit> enemies, allies, camps;
    std::vector<vec2> altars;
    bc::bitboard static_blocked, blocked, visible, altar_goals;
    bc::distance_field home_field, altar_field, origin_field;
    std::vector<bc::distance_field> altar_routes;
    std::vector<AltarAccess> altar_access;
    TeamMemory shared;
    DraftPlan opening;
    SquadState squad;
    JobSearch job_search;
    ObjectiveSearch active_search;
    ClearProgress clear_progress;
    std::array<OriginRoute, TeamMemory::slots> member_routes;
    TravelRoute travel_route, approach_route;
    unsigned terrain_revision = 0;
    bc::bitboard known_camp_obstacles;
    std::vector<float> roster_power;
    std::vector<bc::champion> enemy_roster;
    std::vector<int> enemy_respawn, enemy_spawned;
    float enemy_average_power = 0;

    bool squad_member() const { return squad.contains(slot); }

    // Squad controller (squad/). Stellar, Tavi and Cassi share one job.
    // turn.cpp, coordination.cpp: shared claims, leadership and supervision.
    void play_squad_turn();
    void reconcile_squad();
    void publish_squad(bool reset_focus = false);
    unsigned squad_claim() const;
    unsigned living_core() const;
    const ObservedUnit *observed_member(unsigned member) const;
    bool same_job(unsigned code) const;
    void adopt_job(unsigned code);
    void begin_recovery();
    void enter_selection();
    void supervise_progress();
    void detect_material_events();
    int work_window() const;
    bool relevant_enemy(const ObservedUnit &enemy) const;
    int relevant_threats() const;
    float control_value(const ObservedUnit &enemy) const;
    FiringOpportunity firing(const ObservedUnit &member, const ObservedUnit &enemy,
                             bool next_turn = false) const;
    float collateral_risk(vec2 centre, float radius, int engaged_group = -1) const;
    void squad_focus();
    void squad_priority();
    // selection.cpp, objectives.cpp: job choice and estimates.
    Selection select_squad_job();
    JobEstimate estimate_job(int index, ObjectiveSearch &search);
    int hidden_interference(const CampGroup &group) const;
    FiringPosition firing_position(unsigned member, int camp, bool siege, bc::bitboard reserved,
                                   FiringSearch &search);
    float position_output(const ObservedUnit &member, vec2 tile, const CampGroup &group,
                          const bc::bitboard &sight, int arrival, int &ready) const;
    const bc::distance_field &member_route(unsigned member);
    vec2 objective_staging(int camp);
    bool prepare_objective();
    void record_camp_damage(int camp, float damage);
    void finish_clear_turn(int camp, float expected);
    // movement.cpp, stellar_tavi_cassi.cpp: executing the job.
    MoveResult squad_move(bc::bitboard goals, bool urgent = false);
    void squad_resource_turn();
    void squad_combat();
    void squad_recover(bool supply = false);
    bool needs_recovery() const;
    // banking.cpp: delivery.
    void observe_cargo();
    void squad_bank();
    BankEstimate estimate_bank(int altar);
    BankEstimate choose_bank();
    bool review_banking(bool urgent = false);
    bool can_protect_channel(int duration) const;
    bool channel_protected(vec2 tile, int start, int duration, unsigned guards,
                           const BankEstimate *plan = nullptr, unsigned *committed_guards = nullptr) const;
    bool schedule_bank(BankEstimate &plan, const std::array<unsigned, TeamMemory::slots> &bags,
                       unsigned already_channeling = 0) const;
    bool crossing_ready();
    bool teleport_assignment(vec2 &landing) const;

    // Shared mechanics (mechanics/). Both controllers execute through these.
    // world.cpp: observation and estimates.
    void initialise();
    void refresh_self();
    void prepare_routes();
    void observe();
    void observe_fight();
    void publish(unsigned claim);
    float incoming(vec2 tile, int engaged_group = -1, bool include_camps = true) const;
    float friendly_power(vec2 at) const;
    float enemy_power(vec2 at) const;
    float respawn_threat(vec2 at) const;
    float dps(const CampGroup &g) const;
    float clear_loss(const CampGroup &g, float clear_time) const;
    // navigation.cpp
    bool move_to(bc::bitboard goals, bool urgent = false, bool use_dash = true, int engaged_group = -1,
                 bool allow_teleport = false);
    bool move_step(vec2 tile);
    bool cast_mobility(const bc::distance_field &field, bool urgent, int engaged_group);
    bool health_recovery_needed() const;
    void retreat_to_spawn();
    // combat.cpp, micro.cpp
    bool combat(bool fighting_champions, int farm_group = -1, bool escaping = false);
    float kill_value(const ObservedUnit &enemy) const;
    float linked_risk() const;
    bool attack_combo(bool escaping);
    bool attack_dash(vec2 victim);
    bool advance_attack(vec2 victim);
    bool follow_through();
    // economy.cpp
    void buy_equipment(bool crystal_required = false);
    void refresh_equipment();
    float upgrade_gain(bc::champion_type champion, float hp, float attack, float spell, unsigned gold) const;
    float upgrade_output(unsigned member, unsigned reward, bool siege, float splash) const;
    bool clear_altar_lock(int altar);
    ActionResult deposit_at(int altar);
    void interrupt_deposit();
    void ward_ahead(vec2 goal);

    // Worker controller (workers/): independent policy for Issac, Ryth and
    // Varg. The squad uses the shared mechanics, never these decisions.
    void play_worker_turn();
    void worker_priority();
    void worker_report();
    bool worker_escape();
    int worker_choose_group();
    void worker_farm();
    bool worker_skirmish();
    void worker_update_formation();
    bool worker_reinforce();
    bool worker_tribute();
    void worker_disrupt_tribute();

  public:
    void play_turn();
};
