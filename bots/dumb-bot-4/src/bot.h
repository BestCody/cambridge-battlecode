#pragma once
#include "strategy.h"
#include <variant>

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
    vec2 centre{};
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
    int spell_reach = 0;
    int range = 0, group = -1, gems = 0, stun_time = -1;
    bool stunned = false, channel = false, depositing = false, immune = false;
    bc::champion_type type{};
    explicit ObservedUnit(bc::champion c);
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
    TeamMemory shared;
    DraftPlan opening;
    int contact_group = -1;
    unsigned contact_force = 0;
    std::vector<float> roster_power;
    std::vector<bc::champion> enemy_roster;
    std::vector<int> enemy_respawn;
    float enemy_average_power = 0;

    void initialise();
    void refresh_self();
    void prepare_routes();
    void observe();
    void observe_fight();
    void publish(unsigned claim);
    void update_formation();
    bool reinforce();
    void shop();
    void ward_ahead(vec2 goal);
    float incoming(vec2 tile, int engaged_group = -1, bool include_camps = true) const;
    float friendly_power(vec2 at) const;
    float enemy_power(vec2 at) const;
    float respawn_threat(vec2 at) const;
    float dps(const CampGroup &g) const;
    float clear_loss(const CampGroup &g, float clear_time) const;
    int choose_group();
    bool move_to(bc::bitboard goals, bool urgent = false, bool use_dash = true, int engaged_group = -1);
    bool move_step(vec2 tile);
    bool cast_mobility(const bc::distance_field &field, bool urgent, int engaged_group);
    bool combat(bool fighting_champions, int farm_group = -1, bool escaping = false);
    bool attack_combo(bool escaping);
    bool attack_dash(vec2 victim);
    bool advance_attack(vec2 victim);
    bool follow_through();
    bool skirmish();
    bool escape();
    bool tribute();
    void disrupt_tribute();
    void farm();
    void report();

  public:
    void play_turn();
    void attack_order();
};
