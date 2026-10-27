#pragma once
#include "mechanics/profile.h"
#include "strategy.h"
#include <new>
#include <span>
#include <variant>

class Bot;

struct CampMemory {
    bc::camp unit;
    vec2 position;
    int tile = -1; // Its row-major index, set once the map is known.
    bc::camp_stats stats;
    float health;
    // The last reading and its round, and whether the camp was then known to
    // be asleep: one still asleep the next round has not been hit since (a
    // hit wakes it), so the reading stands.
    int read_round = -10;
    bool read_asleep = false;
    // In view at the group's last quiet observation (see CampGroup::quiet).
    bool quiet_view = false;
    float read_health = 0, read_max_health = 0, read_strength = 0;
    static bc::camp_stats design(const bc::camp &c) {
        // One entry per camp type; sized with room for new types (golem is
        // type 5, past monolith) so a new camp can never write out of range.
        static std::array<std::optional<bc::camp_stats>, 16> cache;
        const auto kind = static_cast<unsigned>(c.kind());
        if (kind >= cache.size())
            return c.base_stats();
        auto &entry = cache[kind];
        if (!entry)
            entry = c.base_stats();
        return *entry;
    }
    explicit CampMemory(const bc::camp &c)
        : unit(c), position(c.spawn_position()), stats(design(c)), health(stats.base_health) {}
};
struct CampGroup {
    // The group's identity and rewards (made without its camp list, which
    // camps holds).
    bc::map::camp_group unit;
    // Its camps, in the Bot's pool of every camp (one allocation for all).
    std::span<CampMemory> camps;
    // The camps' spawn tiles, built on first use: most groups never need one.
    const bc::bitboard &tiles() const {
        if (!tile_mask) {
            tile_mask.emplace();
            for (const auto &c : camps)
                tile_mask->set(c.position);
        }
        return *tile_mask;
    }
    vec2 centre{};
    float max_health = 0, health = 0, pressure = 0, max_pressure = 0, splash = 1, shape_splash = 1,
          full_splash = 1;
    int seen = -1, respawn = 0, attacked = -10000;
    int holding_since = -1;
    // The round of the last observation that found the group alive with every
    // camp in view asleep and living, and its records then (all read or kept
    // asleep); -10 when the last one did not.
    int quiet = -10;
    std::uint16_t quiet_records = 0;
    int home_distance = 0, enemy_distance = 0, altar_distance = 0;
    // The walks from spawn and from the altars to the group's farthest camp
    // (measure_farthest()), for lower bounds on walks to it from anywhere.
    int home_farthest = bc::distance_field::unreachable, altar_farthest = bc::distance_field::unreachable;
    // Whether the full group leaves any tile for a spell-range siege; -1 until
    // first checked (terrain and camp ranges are fixed).
    int siege_open = -1;
    // Walking distances from the group's camps over terrain, built by
    // background work and exported row-major (a read costs a load, where a
    // field's get() costs an engine call). Walking is symmetric, so the entry
    // at p is the distance from p to the nearest camp.
    std::unique_ptr<std::uint16_t[]> walk_tiles;
    enum Status { alive, capture, cooling } status = alive;
    CampGroup(bc::map::camp_group g, std::span<CampMemory> members);
    // The most camps within splash radius of one camp, counted over every pair.
    float count_splash() const;
    // Sets the splash figures from that shape.
    void set_splash(float shape);

  private:
    mutable std::optional<bc::bitboard> tile_mask;
};

// The share of strength x clear time a clear costs the champions clearing it
// (camp choice's risk, the siege decision), by the camps that strike back,
// fitted to measured clears: one 0.68, two 0.5, three or more 0.35 (they die
// one by one, and ranged attackers stay within reach of fewer of them). A flat
// 0.68 for every group of several camps overstated the cost of two- and
// three-monster camps 1.4-3x; the fit won 16 more of 490 games (46 of 70 head
// to head) and raised gems by round 300 from 97 to 107.
inline float camp_loss_factor(const CampGroup &g) {
    int strikers = 0;
    for (const auto &c : g.camps)
        strikers += c.stats.base_strength > 0;
    return strikers <= 1 ? (g.camps.size() > 1 ? 0.68f : 1.0f) : strikers == 2 ? 0.5f : 0.35f;
}
struct ObservedUnit {
    // The roster's champion or the camp group's camp: those never move.
    std::variant<const bc::champion *, const bc::camp *> ref;
    vec2 position;
    float health = 0, max_health = 0, strength = 0, focus = 0, shield = 0, mitigation = 1;
    float damage_rate = 0, burst = 0, ready_shield = 0, healing_rate = 0;
    // The ready damage spells' burst alone (burst also counts a ready
    // attack reset's hit), and whether an attack reset is ready.
    float spell_burst = 0;
    bool reset_ready = false;
    // The ability slots off cooldown, a bit each (exit_damage()).
    std::uint8_t slot_ready = 0;
    int move_speed = 1;
    int spell_reach = 0;
    int range = 0, group = -1, gems = 0, stun_time = -1;
    bool stunned = false, channel = false, immune = false, completed = false, recalling = false, tributing = false;
    // Its dash: tiles, rounds until usable (-1: no dash), cooldown, and
    // whether a walk can follow it in the same turn.
    std::int8_t dash_range = 0, dash_wait = -1, dash_cooldown = 1;
    bool dash_walks = false;
    std::int8_t recall_left = -1; // An enemy's recall's turn ends left (-1: none read).
    bc::champion_type type{};
    // From live fields already read with one engine call (see sighting()).
    ObservedUnit(const bc::champion &c, const bc::detail::champion_info &info);
    // A sleeping camp carries no buffs, so they are read only when awake.
    ObservedUnit(const CampMemory &c, int group_index, const bc::detail::camp_state &info,
                 bool asleep = false);
    // A camp's record made again from a new reading, in place.
    void reread_camp(const bc::detail::camp_state &info, bool asleep);
    // Another camp's record made in place over a camp's record: the fields
    // every camp record shares are never changed, so only its own are written.
    void recamp(const CampMemory &c, int group_index, const bc::detail::camp_state &info, bool asleep);
    const bc::unit &unit() const {
        return std::visit([](const auto *x) -> const bc::unit & { return *x; }, ref);
    }
    const bc::champion &champion() const { return *std::get<const bc::champion *>(ref); }
    const bc::camp &camp() const { return *std::get<const bc::camp *>(ref); }
    bool enemy() const { return std::holds_alternative<const bc::champion *>(ref); }
    float retaliation() const {
        return enemy() && type == bc::champion_type::ren
                   ? focus * bc::constants::passive_abilities::ren_retaliation_focus_ratio
                   : 0;
    }
};

// Records are copied and moved in bulk (the camp list is rebuilt each turn).
static_assert(std::is_trivially_copyable_v<ObservedUnit>);

// The camps in view as records, in observation order. Each group's records
// live in its own slots, made in place when the group is observed, and the
// order lists slots: re-observing a group moves only its slot numbers to the
// end, and a group that has not changed keeps its records where they are
// (records cost about a fuel per byte to write or move).
class CampList {
    struct alignas(ObservedUnit) Slot {
        std::byte bytes[sizeof(ObservedUnit)];
    };
    std::unique_ptr<Slot[]> storage;  // Never filled: each record is made before it is read.
    std::vector<std::uint32_t> first; // Each group's first slot.
    std::vector<std::uint16_t> held;  // Each group's records, from its first slot.
    std::vector<std::uint16_t> made;  // Each group's slots that have held a record, from its first.
    std::vector<std::uint16_t> order;
    std::uint32_t slot_count = 0;
    ObservedUnit *slots() const { return std::launder(reinterpret_cast<ObservedUnit *>(storage.get())); }

  public:
    template <class Unit>
    class Iterator {
        Unit *slots;
        const std::uint16_t *at;

      public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = ObservedUnit;
        using difference_type = std::ptrdiff_t;
        using pointer = Unit *;
        using reference = Unit &;
        Iterator(Unit *slots, const std::uint16_t *at) : slots(slots), at(at) {}
        Unit &operator*() const { return slots[*at]; }
        Unit *operator->() const { return &slots[*at]; }
        Iterator &operator++() {
            ++at;
            return *this;
        }
        Iterator operator++(int) {
            Iterator before = *this;
            ++at;
            return before;
        }
        bool operator==(const Iterator &other) const { return at == other.at; }
        bool operator!=(const Iterator &other) const { return at != other.at; }
    };
    Iterator<ObservedUnit> begin() { return {slots(), order.data()}; }
    Iterator<ObservedUnit> end() { return {slots(), order.data() + order.size()}; }
    Iterator<const ObservedUnit> begin() const { return {slots(), order.data()}; }
    Iterator<const ObservedUnit> end() const { return {slots(), order.data() + order.size()}; }
    std::size_t size() const { return order.size(); }
    bool empty() const { return order.empty(); }
    ObservedUnit &operator[](std::size_t i) { return slots()[order[i]]; }
    const ObservedUnit &operator[](std::size_t i) const { return slots()[order[i]]; }
    // A group's records, in camp order (none for no group).
    std::span<ObservedUnit> group(int g) {
        return g < 0 ? std::span<ObservedUnit>() : std::span<ObservedUnit>(slots() + first[g], held[g]);
    }
    std::span<const ObservedUnit> group(int g) const {
        return g < 0 ? std::span<const ObservedUnit>()
                     : std::span<const ObservedUnit>(slots() + first[g], held[g]);
    }
    std::uint16_t count(int g) const { return held[g]; }
    // A record by its slot, and the group's first slot and the one its next
    // record takes.
    ObservedUnit &slot(std::uint32_t s) { return slots()[s]; }
    const ObservedUnit &slot(std::uint32_t s) const { return slots()[s]; }
    std::uint32_t first_slot(int g) const { return first[g]; }
    std::uint32_t next_slot(int g) const { return first[g] + held[g]; }
    std::uint32_t slots_total() const { return slot_count; }
    // Slots for every camp of these groups; none listed.
    void reset(const std::vector<CampGroup> &groups) {
        first.resize(groups.size());
        held.assign(groups.size(), 0);
        made.assign(groups.size(), 0);
        std::uint32_t total = 0;
        for (std::size_t g = 0; g < groups.size(); ++g) {
            first[g] = total;
            total += std::uint32_t(groups[g].camps.size());
        }
        storage.reset(new Slot[total]);
        slot_count = total;
        order.clear();
        order.reserve(total);
    }
    // A new observation lists nothing yet; records stay until rewritten.
    void clear() { order.clear(); }
    // The group's records are rewritten from its first slot (call when they
    // are not listed: after clear() or drop()).
    void restart(int g) { held[g] = 0; }
    // The group's next record, a camp's, made in its slot and listed last
    // (over the camp record the slot holds, if any, writing only its own fields).
    ObservedUnit &emplace(int g, const CampMemory &c, const bc::detail::camp_state &info, bool asleep) {
        const std::uint32_t slot = first[g] + held[g]++;
        order.push_back(std::uint16_t(slot));
        if (held[g] > made[g]) {
            made[g] = held[g];
            return *new (&storage[slot]) ObservedUnit(c, g, info, asleep);
        }
        auto &record = slots()[slot];
        record.recamp(c, g, info, asleep);
        return record;
    }
    // List the group's next record as it stands in its slot.
    void keep_next(int g) { order.push_back(std::uint16_t(first[g] + held[g]++)); }
    // List the group's first records again as they stand.
    void keep(int g, std::uint16_t records) {
        held[g] = records;
        for (std::uint32_t slot = first[g]; slot < first[g] + records; ++slot)
            order.push_back(std::uint16_t(slot));
    }
    // Take the group's records out of the order, where they are listed together.
    void drop(int g) {
        if (!held[g])
            return;
        const std::uint32_t low = first[g], high = low + held[g];
        const auto at = std::find_if(order.begin(), order.end(),
                                     [&](std::uint16_t slot) { return slot >= low && slot < high; });
        if (at != order.end())
            order.erase(at, at + held[g]);
    }
};

// Engine reads (world.cpp). One call returns every live field of a champion or
// camp, where the SDK's accessors repeat that call for each field.
bc::detail::champion_info read_champion(const bc::champion &c);
// The live fields of a champion in view; nothing when it is not visible.
std::optional<bc::detail::champion_info> sighting(const bc::champion &c);
// False when the camp is not visible.
bool read_camp(const bc::camp &c, bc::detail::camp_state &info);
// 1 alive, 2 reward available, 3 respawning (timer set), for a group in view.
int group_state(const CampGroup &g, int &timer);
// The unit's health now, or 0 when it is out of sight.
float live_health(const ObservedUnit &u);
// The unit's stun and shield now (the shield kept when it has none).
void read_buffs(ObservedUnit &out);
// The unit's shield now, 0 when it has none.
float live_shield(const ObservedUnit &u);
// Our own shield now, 0 when we have none (one engine read; the SDK's
// buffs() decodes every record).
float self_shield();

// What a worker spent each turn on, for the time-use diagnostic.
enum class Activity { idle, dead, recall, heal, retreat, bank, fight, reinforce, farm, travel, count };

// One bit per tile, row-major, from the engine's packed export of a mask: one
// call of about 200 fuel, where testing tiles one by one costs a call each.
class TileBits {
    // The export writes every byte, so the buffer is never zero-filled (memory
    // fills cost about a fuel per byte).
    std::unique_ptr<std::uint8_t[]> bits;

  public:
    void load(const bc::bitboard &mask) {
        const auto size = bc::bitboard::linear_buffer_size();
        if (!bits)
            bits.reset(new std::uint8_t[size]);
        mask.export_linear_buffer(std::span(bits.get(), size));
    }
    bool operator[](int index) const { return bits[index >> 3] >> (index & 7) & 1; }
};

// Diagnostic snapshot of this champion's last chosen state, reported when it
// is next seen dead. Explains what each death was exposed to.
struct LifeSnapshot {
    int round = -1;
    vec2 position{-1, -1};
    float health = 0, max_health = 0, enemy_threat = 0, camp_threat = 0;
    unsigned gems = 0;
    int enemies_near = 0, nearest_enemy = -1, phase = -1, job = -1;
    bool retreating = false;
};

// Thrown when our own champion is off the map in the middle of its turn: one
// of its actions killed it (a dash landing beside retaliation, say). Nothing
// is left to do this turn, and every position-based query would fail.
struct ChampionGone {};

class Bot {
    bool initialised = false, walked = false, retreating = false, banking = false;
    // Hit since our last turn, by anything, seen or not (a camp out of view,
    // an enemy in a bush), and where our last recall started.
    bool hit = false;
    vec2 recall_tile{-1, -1};
    // This contact's first round and the last round with an enemy in view (worker_skirmish).
    int contact_start = -100, contact_last = -100;
    // Fuel at the start of this turn: the work done since is the same at any
    // budget, so the background pacing uses it (turn_work()), never the fuel
    // left.
    std::uint64_t turn_start_fuel = 0;
    std::uint64_t search_job_cost = 0; // The last map-wide search's work.
    std::uint64_t camp_count = 0;
    std::uint64_t turn_work() const { return turn_start_fuel - bc::fuel_remaining(); }
    unsigned slot = 0, round = 0;
    int target = -1, last_plan = -10000, last_report = -1000, stalls = 0;
    // Walled off (camp bodies sealing the only route): turns of a walk that
    // made no step, and the group or altar left out until a round.
    int farm_stalls = 0, stuck_group = -1, stuck_group_until = -1;
    int bank_stalls = 0, stuck_altar = -1, stuck_altar_until = -1;
    int zone_target = -2; // The target mark_camp_zones() exempted.
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
    std::vector<CampMemory> camp_pool; // Every group's camps; never grows once made.
    // Each group's centre (fixed), apart: a scan of them reads two arrays.
    std::vector<std::int32_t> group_cx, group_cy;
    // Every camp's tile apart and its group, in camp_pool's order, padded to
    // camp_lanes entries (a multiple of four) with tiles far away: a scan of
    // every camp reads three arrays.
    std::unique_ptr<std::int32_t[]> camp_x, camp_y;
    std::unique_ptr<std::uint16_t[]> camp_group;
    std::size_t camp_lanes = 0;
    std::vector<bc::champion> roster;
    std::vector<ObservedUnit> enemies, allies;
    // Each ally in view's roster slot, in allies' order, and those slots' bits.
    std::array<std::uint8_t, TeamMemory::slots> ally_slot{};
    unsigned seen_slots = 0;
    // Each enemy in view's entry in enemy_roster, in enemies' order.
    std::array<std::uint8_t, 8> enemy_slot{};
    // This round's fight report: the group nearest the enemy we see and the
    // squad it takes (-1: no enemy in view).
    int reported_round = -1, reported_group = -1;
    unsigned reported_force = 0;
    // Enemies out of sight (safety.cpp), per enemy roster entry: the round
    // its record was last brought up to date (-1: none), its last sighting
    // and that sighting's round, whether its tiles have been made from that
    // sighting yet, and the tiles within our sight radius where it could be
    // now. This turn's records of those that could be near, each placed on
    // its possible tile nearest us, and in the same order the tiles they
    // were last seen on.
    std::vector<int> ghost_round, ghost_sighted;
    std::vector<std::optional<ObservedUnit>> ghost_seen;
    std::vector<std::uint8_t> ghost_built;
    std::vector<bc::bitboard> ghost_where;
    std::vector<ObservedUnit> ghosts;
    std::vector<vec2> ghost_last;
    int ghost_update = -1; // The round the records were last brought up to date.
    int enemy_sighted = -1000; // The last round an enemy was in view.
    // Enemy recalls (observe()), per enemy roster entry: its turn ends up to
    // the last sighting of it recalling, the turn ends that recall had left
    // then, and whether it was counting down (a real one) or just started.
    std::array<int, 8> recall_tick{};
    std::array<std::int8_t, 8> recall_seen{};
    std::array<bool, 8> recall_real{};
    // This turn ends in a recall to be cancelled as the next one starts
    // (fake_recall()), unless it carries a capture's holder home
    // (home_after_capture()).
    bool faking = false, homeward = false, capture_recall = false;
    // The capture group we stand on (worker_farm()) and the round we stood on
    // it from (-1: none).
    int capture_group = -1, capture_since = -1;
    // The group we close in on beside its nearest camp, none of its camps in
    // sight from a tile in range (worker_farm(); -1: none).
    int unseen_group = -1;
    // The last round we stood on our spawn (home_after_capture()).
    int left_spawn = 0;
    // The Bot's masks are made without zeroing (a clear costs a whole-mask
    // operation): each is written in full or cleared before it is read.
    bc::bitboard ghost_keep{bc::uninitialized}, ghost_spread{bc::uninitialized};
    std::vector<vec2> ghost_tiles;
    // The round avoid_reach() kept this turn's moves out of a stronger
    // group's reach, and those tiles.
    int avoid_round = -1;
    bc::bitboard avoid_zone{bc::uninitialized};
    bc::bitboard camp_zone{bc::uninitialized};
    // Tiles a dash landing would strike a camp from (drop_waking_landings()).
    bc::bitboard dash_wake{bc::uninitialized};
    int camp_zone_round = -1;
    // Breaking contact with a stronger group (safety.cpp): whether we run or
    // fight, decided once and kept until something the decision did not
    // foresee happens. The round it was decided and last kept, the enemies
    // in view then (bits of their roster entries), whether the run
    // uses our dash and whether it was ready, the teammate it runs to (its
    // roster slot, -1: spawn), our health and shield then, and the damage the
    // race or duel foresaw by the end of each round since.
    struct Chase {
        enum Mode { none, run, fight };
        Mode mode = none;
        int decided = -1, kept = -1, mate = -1;
        std::uint32_t chasers = 0;
        bool dash = false, dash_ready = false;
        float pool = 0;
        std::vector<float> foreseen;
    } chase;
    CampList camps;
    std::vector<ObservedUnit> fight_scratch; // observe_fight()'s next enemies
    // The camps that can strike (positive strength, no stun with time left),
    // as their records' slots in camps'
    // order, listed as each observation lists the records, for incoming();
    // with their tiles and reach as 16-bit lanes padded to eights. Sized
    // once for every camp: threat_count are listed, threat_lanes with the
    // padding.
    std::unique_ptr<int[]> camp_threats;
    std::unique_ptr<std::int16_t[]> threat_x, threat_y, threat_reach;
    std::size_t threat_count = 0, threat_lanes = 0;
    // Per threat, the distance from its camp to the nearest ally or enemy in
    // view, worked out on first use and kept while threat_revision stands
    // (the units and the threat list are the same).
    mutable std::unique_ptr<int[]> threat_nearest;
    // And how many other units stand at that distance.
    mutable std::unique_ptr<int[]> threat_ties;
    mutable std::vector<std::uint32_t> threat_stamp;
    std::uint32_t threat_revision = 1;
    // Every ally's and enemy's tile, padded to fours far away, for the
    // nearest unit to a camp.
    mutable std::vector<std::int32_t> unit_x, unit_y;
    mutable std::uint32_t unit_revision = 0;
    // The enemies that can strike (not stunned), in order, packed for
    // incoming() in lanes of four: tile, reach for a hit, for a melee lunge
    // (one more; none for ranged) and for a spell, and what each adds. Padding
    // reaches no tile.
    struct EnemyLanes {
        std::vector<std::int32_t> x, y, reach, lunge, spell;
        std::vector<float> strength, lunge_strength, half_burst;
        std::size_t lanes = 0;
        std::uint32_t revision = 0;
    };
    mutable EnemyLanes enemy_lanes;
    // The enemies exit_damage() counts (in view, then ghosts; none dead or
    // channelling), packed in lanes of four and kept while threat_revision
    // and the ghosts stand: tile; the rounds a stun holds it; the reach of
    // its hits while we leave and of its first-round strike; the reach of
    // its two slots' ready bursts and of a ready stun on us (-1: none); its
    // hit and the two bursts. Padding reaches no tile. And the most all of
    // them could deal (each burst, and a hit for every round of its reach
    // and a stunned one) and their hits in one round.
    struct ExitLanes {
        std::vector<std::int32_t> x, y, held, reach, lunge, reach_a, reach_b, stun;
        std::vector<float> hit, burst_a, burst_b;
        std::size_t lanes = 0;
        float most = 0, hits = 0;
        std::uint32_t revision = 0;
        int ghosts = -1;
    };
    mutable ExitLanes exit_lanes;
    std::vector<vec2> altars;
    // Altar locks (tribute.cpp), per altar: the last round an enemy was seen
    // banking at it and the last round a lock was seen on it (-1000: never);
    // and whether we carry a lock.
    std::vector<int> altar_enemy_bank, altar_locked;
    bool carrying_lock = false;
    bc::bitboard static_blocked{bc::uninitialized}, blocked{bc::uninitialized}, visible{bc::uninitialized},
        altar_goals{bc::uninitialized};
    bc::distance_field home_field, altar_field, origin_field;
    vec2 home_centre{}; // The middle of our spawn region.
    std::vector<bc::distance_field> altar_routes;
    TeamMemory shared;
    DraftPlan opening;
    std::vector<float> roster_power;
    // Each roster slot's kit, camp damage multiplier and base damage per
    // round on camps.
    struct RosterKit {
        std::array<Spell, 2> spells;
        float camp_multiplier = 1, camp_rate = 0;
    };
    std::vector<RosterKit> roster_kits;
    // Camp choice's workspace, per group its claimants' summed camp rates.
    std::vector<float> claim_rates;
    // The capture gold multiplier (Vesper's passive, Treasure).
    float gold_bonus = 1;
    // Turn ends our recall takes: five, three with a Home emblem.
    int recall_turns = 5;
    std::vector<bc::champion> enemy_roster;
    std::vector<int> enemy_respawn;
    float enemy_average_power = 0;
    LifeSnapshot last_life;
    // Shards this champion has bought, by item type, for its build order.
    std::array<unsigned, static_cast<unsigned>(bc::item_type::count)> bought{};
    // Health lost while an enemy champion could hit us (a death counts what
    // was left), which picks the first emblem's stat; whether one could last
    // turn, and whether this death is counted.
    float champion_damage = 0;
    // champion_damage at the last three 50-round marks (buy_equipment()).
    std::array<float, 3> damage_marks{};
    int damage_mark_round = 0;
    bool threatened_last = false, death_counted = false;
    // Per camp group: last round this worker saw an enemy close to it.
    std::vector<int> enemy_seen_near;
    std::array<unsigned, static_cast<unsigned>(Activity::count)> activity_turns{};
    Activity current_activity = Activity::idle;
    int last_activity_round = -1;
    unsigned target_switches = 0;
    // Engine queries cost fuel per call, so per-tile tests in hot loops read
    // these copies instead. Row-major, indexed by tile_index().
    int map_width = 0, map_height = 0;
    // Immutable: camp tiles and the group on each (tile_group is written, and
    // read, only where camp_tile is set), bush tiles, tiles no area spell may
    // be centred on (walls, holes and altars), and the enemy spawn.
    std::unique_ptr<std::uint16_t[]> tile_group;
    TileBits camp_tile, bush_tile, no_centre_tile, enemy_spawn_tile;
    // Camps in view, refreshed by each observation (with the sleeping ones
    // when asleep_known, and the living ones); unseen ward placements.
    TileBits camp_in_view, camp_asleep, camp_living, unseen_tile;
    TileBits path_reached; // measure_paths()'s search: the tiles it reached.
    bool asleep_known = false;
    // Per group, with each observation: a camp of it in view, and one of us
    // (or an ally in view) standing on it. The camp tiles in view, listed.
    std::vector<std::uint8_t> group_in_view, group_held;
    std::vector<vec2> sighted_camps;
    // Walking distance from origin_position to each camp group. A group a
    // bounded search missed holds only a lower bound, flagged in
    // group_bounded; path_search is the furthest level searched from here.
    std::vector<int> group_path;
    std::vector<std::uint8_t> group_bounded;
    int path_search = 0;
    // Camps of spawn-reachable groups still without a walk field: the
    // destinations of the search from here. Its distances, exported.
    bc::bitboard unwalked_camps{bc::uninitialized};
    std::unique_ptr<std::uint16_t[]> path_buffer;
    // Background work: the altar field is built on demand (see altar_walk()),
    // and groups' walk fields in walk_order, of which walk_cursor are done.
    bool altar_field_ready = false;
    std::vector<int> walk_order;
    std::size_t walk_cursor = 0;
    // Each group's exported walk field once built (its walk_tiles), else null:
    // one array to scan instead of every group's record.
    std::vector<const std::uint16_t *> walk_of;
    // Reused masks and fields: a new mask costs about 600 fuel, as much as six
    // in-place operations. Each is written in full (or cleared) before it is
    // read; the searches' visited mask starts cleared.
    bc::bitboard open_terrain{bc::uninitialized};  // ~static_blocked
    bc::bitboard occupancy{bc::uninitialized};     // occupied_into()'s workspace
    bc::bitboard search_origin{bc::uninitialized}; // one-tile search origins
    bc::bitboard search_goals{bc::uninitialized};  // goals minus the blocked tiles
    bc::bitboard search_visited;
    bc::bitboard step_tiles{bc::uninitialized}; // reachable tiles of a step or dash
    std::vector<vec2> step_list;                // step_positions()'s storage
    bc::bitboard move_goals{bc::uninitialized}; // move_to()'s goals on open terrain
    bc::bitboard farm_goals{bc::uninitialized}; // worker_farm()'s standing tiles
    bc::bitboard dash_blocked{bc::uninitialized};
    bc::bitboard attack_area{bc::uninitialized};
    std::vector<bc::bitboard> sight_pool; // the fight search's vision per origin
    // move_to()'s field, and the field each group's walk is built in before
    // its export.
    bc::distance_field move_field, walk_field;
    // A dash's landings and their walks: the landings in reading order, and
    // the walk field exported row-major (every entry written before reading).
    std::vector<vec2> dash_landings;
    std::unique_ptr<std::uint16_t[]> dash_walks;
    // Spell reach for sieges (0 without a range-four damage spell), and the
    // next group whose siege_open the background work checks.
    float siege_reach = 0;
    std::size_t siege_cursor = 0;
    // Every camp's tile; walls, altars and the enemy spawn, which no dash may
    // cross or land on; and the enemy spawn's tiles.
    bc::bitboard camp_tiles{bc::uninitialized}, dash_terrain{bc::uninitialized};
    std::vector<vec2> enemy_spawn_tiles;
    // Items changed since the last stat refresh: only shopping changes them.
    bool equipment_dirty = true;
    int tile_index(vec2 p) const { return p.y * map_width + p.x; }
    int group_at(vec2 p) const {
        if (p.x < 0 || p.y < 0 || p.x >= map_width || p.y >= map_height)
            return -1;
        const int index = tile_index(p);
        return camp_tile[index] ? int(tile_group[index]) : -1;
    }
    // Terrain plus every visible occupant, without our own tile.
    void occupied_into(bc::bitboard &out, const bc::bitboard &terrain);
    void refresh_blocked() {
        occupied_into(blocked, static_blocked);
        if (avoid_round == int(round))
            blocked |= avoid_zone;
        if (camp_zone_round == int(round))
            blocked |= camp_zone;
    }
    // The reach of awake big camps of other groups, no way through this turn.
    void mark_camp_zones();
    // out = a & ~b, in place.
    void subtract_into(bc::bitboard &out, const bc::bitboard &a, const bc::bitboard &b);
    // a &= ~b, in place.
    void subtract(bc::bitboard &a, const bc::bitboard &b);
    // A one-tile mask at p, in search_origin.
    const bc::bitboard &origin_at(vec2 p);

    // Shared mechanics (mechanics/).
    // world.cpp: observation and estimates.
    void initialise();
    void record_life();
    void report_death();
    void refresh_self();
    const bc::distance_field &altar_walk();
    // Searches to path_bound (level 1), twice it (2) or completely (3).
    void measure_paths(int level);
    void measure_farthest(const bc::distance_field &field, int CampGroup::*farthest);
    bool background_search();
    void prepare_routes();
    void observe();
    // A sighted enemy's recall, real or faked (cancelled every turn), by its
    // countdown since the last sighting (the enemy's roster entry): a real
    // one is a channel.
    void judge_recall(ObservedUnit &enemy, std::size_t entry);
    void observe_after_move(int group);
    void observe_group(std::size_t index);
    // With a group, only what observe_group() reads for that one group.
    void mark_camps_in_view(int group = -1);
    void mark_group_holders(int group = -1);
    void list_threat(std::uint32_t slot);
    void drop_threats(int group);
    void close_threats();
    int nearest_unit(vec2 tile) const;
    // Also the number of units at that distance.
    int nearest_unit(vec2 tile, int &ties) const;
    void observe_fight();
    void publish(unsigned claim);
    float incoming(vec2 tile, int engaged_group = -1, bool include_camps = true) const;
    // The damage enemy champions deal a champion on `tile` that leaves now,
    // a tile a round, until it is out of every one's reach, before its
    // mitigation (exit_lanes): each ready burst that reaches it next round
    // lands once (a second the round after: one cast a turn); each enemy
    // hits once a round while it is within that enemy's reach (one stunned
    // now starts when its stun ends), a ready dash or reset that brings a
    // strike within reach adding one; a ready stun on it (none when
    // `immune`) holds it a round, a hit more from each enemy that reaches
    // it. An enemy on `without` is left out. And one round of the hits of
    // the enemies that hit it at all.
    struct ExitDamage {
        float damage = 0, round = 0;
    };
    ExitDamage exit_damage(vec2 tile, bool immune, vec2 without = {-1, -1}) const;
    // exit_lanes, made again when stale.
    const ExitLanes &exit_packed() const;
    // Exit damage and the bag margin (a round of those hits per prism_bag
    // gems carried), after `mitigation`; both 0 when the most the enemies
    // could deal with that margin stays under `floor` (a lower bound of
    // what they are weighed against: the answer is the same).
    struct ExitRisk {
        float damage = 0, margin = 0;
    };
    ExitRisk exit_risk(vec2 tile, float floor, float mitigation, unsigned carried, bool immune) const;
    // Our health, shield and a ready shield spell's (cast as we walk), given
    // our shield and slot readiness, or read here.
    float exit_pool(float shield, const std::array<bool, 2> &is_ready) const;
    float exit_pool() const;
    // A step or landing onto a tile whose exit damage reaches our pool and
    // is worse than here's: the micro refuses it. The pool and the damage
    // here are read on first need.
    struct ExitCheck {
        const Bot &bot;
        float pool = -1, here = 0;
        bool lethal(vec2 tile);
    };
    float friendly_power(vec2 at) const;
    // Our own power alone (friendly_power without teammates).
    float own_power() const;
    float enemy_power(vec2 at) const;
    float respawn_threat(vec2 at) const;
    float dps(const CampGroup &g) const;
    float clear_loss(const CampGroup &g, float clear_time) const;
    // Whether a visit to the group can afford the walk out of its reach
    // under its fire (camp choice's visit_health of 100+).
    bool visit_affordable(const CampGroup &g) const;
    // navigation.cpp
    // Of equally short steps, the one nearest `toward` in a straight line.
    bool move_to(const bc::bitboard &goals, vec2 toward, bool urgent = false, bool use_dash = true,
                 int engaged_group = -1, bool allow_teleport = false);
    // step_tiles' tiles in reading order, listed into storage kept between
    // calls (positions() allocates a list on each): step_tiles must hold
    // tiles within our move speed on both axes, as a step's reach does.
    std::span<const vec2> step_positions();
    bool move_step(vec2 tile);
    bool cast_mobility(const bc::distance_field &field, bool urgent, int engaged_group);
    // Drops from `landings` those a damaging dash's landing (radius) would
    // strike a living camp from, for camps within `reach` of us of groups
    // other than our target and `keep`.
    void drop_waking_landings(bc::bitboard &landings, int reach, float radius, int keep);
    bool health_recovery_needed() const;
    void retreat_to_spawn();
    // combat.cpp, micro.cpp
    bool combat(bool fighting_champions, int farm_group = -1, bool escaping = false);
    float kill_value(const ObservedUnit &enemy) const;
    // How much an enemy diving a teammate counts in our choices (1: no dive).
    float peel_factor(const ObservedUnit &enemy) const;
    bool attack_combo(bool escaping);
    bool attack_dash(vec2 victim);
    bool impact_dash(bool fighting_champions, int farm_group);
    bool advance_attack(vec2 victim, int stand = 0);
    bool follow_through();
    // economy.cpp
    void buy_equipment();
    void refresh_equipment();
    void ward_ahead(vec2 goal);

    // Worker controller (workers/): every champion's independent policy.
    void play_worker_turn();
    void worker_priority();
    void fake_recall();
    void worker_report();
    bool worker_escape();
    // excluded: a group never chosen (-1: none).
    int worker_choose_group(int excluded = -1);
    void worker_farm();
    bool capture_outranked(const CampGroup &g) const;
    // Whether an ally holds a capture before us (capture_outranked()).
    bool outranks(const ObservedUnit &ally, bool arriving) const;
    // An ally whose published code asks for help in a fight.
    bool reports_fight(const ObservedUnit &ally) const;
    // The round a capture we hold pays (its reward's capture phase).
    int capture_payout(const CampGroup &g) const;
    // Whether a teammate near a capture holds it from the start instead of us.
    bool capture_deferred(const CampGroup &g) const;
    void note_altar_locks();
    bool lock_wanted(std::size_t altar) const;
    void place_lock();
    bool worker_pursue();
    bool home_after_capture(const CampGroup &g) const;
    float basic_damage() const;
    void track_enemy_respawns();
    bool camp_dormant(const ObservedUnit &c) const;
    int claimed_by(const ObservedUnit &ally) const;
    bc::bitboard siege_tiles(const CampGroup &g, float reach, bool assume_alive) const;
    void prepare_sieges();
    bool ally_draws_attacks(const CampGroup &g) const;
    bool worker_skirmish();
    // Out of a fight too dangerous to stay in (worker_skirmish()).
    bool leave_fight(vec2 victim);
    // Reports the fight we see (true) or clears an old report.
    bool worker_update_formation();
    float team_average_power() const;
    // A fight report's group describes the fight at this tile.
    bool same_fight(int group, vec2 at) const;
    // The squad the fight at this tile takes, and the enemy power teammates'
    // reports of it stand for.
    unsigned fight_demand(vec2 at) const;
    float reported_power(vec2 at) const;
    bool worker_reinforce();
    bool worker_tribute();
    void note_activity(Activity activity);
    // safety.cpp: danger from enemy champions, remembered sightings, and
    // stepping away.
    bool enemy_threatens(const ObservedUnit &enemy, vec2 tile) const { return enemy_threatens(enemy, enemy.position, tile); }
    // As if the enemy stood on `at`.
    bool enemy_threatens(const ObservedUnit &enemy, vec2 at, vec2 tile) const;
    void track_ghosts();
    void worker_track_sightings();
    std::pair<int, int> worker_danger(vec2 tile) const;
    // The tile a step away from danger takes (our own: none).
    vec2 step_away_tile();
    // A melee champion's strike, from where it stands, before a step away.
    void strike_before_step();
    // Run or fight while breaking contact with a stronger group.
    Chase::Mode chase_mode(bool retreat);
    std::uint32_t threat_bits() const;
    bool duel_won(std::uint32_t threats, float pool);
    bool race(vec2 goal, int arrive, bool use_dash, float pool);
    bool run_to_safety();
    void avoid_reach();

    void take_turn();

  public:
    void play_turn();
};
