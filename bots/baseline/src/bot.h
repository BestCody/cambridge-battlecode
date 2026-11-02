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
    // The last round the engine reported its capture contested while we held
    // it (observe_group()): an enemy on the tiles, in view or not.
    int contested = -1;
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
    // Facts every reader shares, of the whole group (as it respawns) and of
    // its living camps (refresh_live(), whenever their health is read):
    // hard (a camp of policy::hard_camp_health or more: a party's clear),
    // the longest camp reach, and the strikers' work (striker_work()).
    // Camp choice and farming read the living ones while the group lives:
    // a group whose golem was dead was a hard party wait for the choice
    // and none for the farming.
    bool hard = false, live_hard = false;
    int reach = 0, live_reach = 0;
    float whole_work = 0, live_work = 0;
    // The share of the strikers' work that reaches us when we clear the
    // group alone from our attack range (Bot::initialise()): only camps whose
    // reach covers the tile we strike from hit us.
    float reach_share = 1;
    void refresh_live();
    CampGroup(bc::map::camp_group g, std::span<CampMemory> members);
    // The most camps within splash radius of one camp, counted over every pair.
    float count_splash() const;
    // Sets the splash figures from that shape.
    void set_splash(float shape);

  private:
    mutable std::optional<bc::bitboard> tile_mask;
};

// The camp damage of a clear per unit of clearing rate (camp choice's risk,
// the siege decision): combat() hits the camps that strike back first, the
// most strength per health first, so each strikes until the health cleared
// before it and its own is gone; the sum of strength x that health, divided
// by the rate, is the clear's camp damage. Harmless camps cost nothing. A
// fitted share of strength x the whole clear (0.68, 0.5, 0.35 by strikers)
// stood in while the harmless clusters went first: the elk beside two
// clusters struck through the whole clear, and one beside twenty would have.
// Whole: the group as it respawns. A group has few strikers: each goes into
// place as it is found (no sort call).
inline float striker_work(const CampGroup &g, bool whole) {
    static std::vector<std::pair<float, float>> strikers; // Strength, health.
    strikers.clear();
    for (const auto &c : g.camps) {
        const float health = whole ? float(c.stats.base_health) : c.health;
        if (c.stats.base_strength <= 0 || health <= 0)
            continue;
        const std::pair<float, float> s{float(c.stats.base_strength), health};
        std::size_t k = strikers.size();
        strikers.push_back(s);
        for (; k > 0 && s.first * strikers[k - 1].second > strikers[k - 1].first * s.second; --k)
            strikers[k] = strikers[k - 1];
        strikers[k] = s;
    }
    float work = 0, cleared = 0;
    for (const auto &[strength, health] : strikers) {
        cleared += health;
        work += strength * cleared;
    }
    return work;
}
struct ObservedUnit {
    // The roster's champion or the camp group's camp: those never move.
    std::variant<const bc::champion *, const bc::camp *> ref;
    vec2 position;
    float health = 0, max_health = 0, strength = 0, focus = 0, shield = 0, mitigation = 1;
    float damage_rate = 0, burst = 0, ready_shield = 0, healing_rate = 0;
    // The share of camp damage it takes (a Jungle emblem; 1 without).
    float camp_mitigation = 1;
    // The damage of its spells cast since our last turn (cooldown within two
    // rounds of full), which a hit on us since may hold.
    float cast_damage = 0;
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
    // An unseen enemy's record, its fields filled in by whoever makes it
    // (note_hidden_hit()).
    explicit ObservedUnit(const bc::champion &c) : ref(&c) {}
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
    // Every group's record count, by group (observe()'s group scan).
    const std::uint16_t *counts() const { return held.data(); }
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

// The damage model's building blocks (mechanics/damage.h).
#include "mechanics/damage.h"

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
    // The micro placed us this turn (worker_skirmish()): its tile is final.
    bool placed = false;
    // Hit since our last turn, by anything, seen or not (a camp out of view,
    // an enemy in a bush), and where our last recall started.
    bool hit = false;
    vec2 recall_tile{-1, -1};
    // Fuel at the start of this turn: the work done since is the same at any
    // budget, so the background pacing uses it (turn_work()), never the fuel
    // left.
    std::uint64_t turn_start_fuel = 0;
    std::uint64_t search_job_cost = 0; // The last map-wide search's work.
    std::uint64_t camp_count = 0;
    std::uint64_t turn_work() const { return turn_start_fuel - bc::fuel_remaining(); }
    unsigned slot = 0, round = 0;
    int target = -1, last_plan = -10000, last_report = -1000, stalls = 0;
    int bounces = 0;
    vec2 before_previous{-1, -1}; // Our position at the start of the turn before last (bounce_stalls)
    // The group camp choice planned a spell-range siege of (-1: none), for
    // worker_farm() to stand as planned.
    int siege_group = -1;
    // The group camp choice planned as a party (-1: none): worker_farm()
    // asks teammates to come.
    int request_group = -1;
    // The team's opening group (choose_opening(); -2 until chosen, -1:
    // none), and whether the opening is over.
    int opening_group = -2, opening_until = 0;
    bool opening_done = false;
    int openings_made = 0, opening_last = -1; // later opening groups (strategy.h opening_groups)
    int rally_holds = 0;
    int own_deaths = 0; bool was_dead = false; // feed_guard (strategy.h) // turns held for the team during the opening (strategy.h rally_radius)
    int choose_opening(int &until) const;
    // Damage taken while tributing, and the turns of the tribute so far;
    // the damage taken over our last turn.
    float tribute_taken = 0, taken_last = 0;
    int tribute_turns = 0;
    // The round worker_skirmish() last declined the fight in
    // play_worker_turn(): nothing acts between that and the tribute's ask
    // (worker_tribute()), so the answer stands without a second run.
    int skirmish_declined = -1;
    // Walled off (camp bodies sealing the only route): turns of a walk that
    // made no step, and the group or altar left out until a round.
    int farm_stalls = 0, stuck_group = -1, stuck_group_until = -1;
    int bank_stalls = 0, stuck_altar = -1, stuck_altar_until = -1;
    int zone_target = -2; // The target mark_camp_zones() exempted.
    vec2 previous{-1, -1}, position{}, ward{-1, -1}, origin_position{-1, -1};
    float health = 0, max_health = 0, strength = 0, focus = 0, camp_multiplier = 1, camp_mitigation = 1,
          mitigation = 1;
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
    // Each ally in view's roster slot, in allies' order, and those slots'
    // bits; and each slot's entry in allies (-1: out of view).
    std::array<std::uint8_t, TeamMemory::slots> ally_slot{};
    unsigned seen_slots = 0;
    std::array<int, TeamMemory::slots> slot_ally{};
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
    // An enemy out of sight that hit us (note_hidden_hit()), on the unseen
    // tile it struck from, kept until that tile is seen, an enemy comes into
    // view or we are a sight radius away; listed after the sightings' ghosts
    // (sighted_ghosts of them).
    std::optional<ObservedUnit> hidden;
    std::size_t sighted_ghosts = 0;
    // The round of its last hit, the enemies out of view then (roster bits:
    // one of them struck), what camps could have struck us this turn
    // (play_worker_turn()), and the last round an enemy died.
    int hidden_hit = -1;
    std::uint32_t hidden_from = 0;
    float camp_bound = 0;
    int enemy_died = -1;
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
        int decided = -1, kept = -1;
        std::uint32_t chasers = 0;
        bool dash_ready = false;
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
    // The enemies that can strike (not stunned through their turn, not
    // teleporting), in order, packed for incoming() in lanes of four: tile,
    // reach for a hit, for a step and a hit (a melee one, or a ranged one
    // with a ready reset; -1 none), whether it is melee (-1) and for a
    // spell; its hit, a ready reset's second hit and its ready burst.
    // Padding reaches no tile.
    struct EnemyLanes {
        std::vector<std::int32_t> x, y, reach, lunge, melee, spell;
        std::vector<float> hit, reset, burst;
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
    vec2 enemy_centre{}; // The middle of the enemy's.
    // A centre map: one group holds centre_share of the map's gems and is
    // opening_reach or more steps from our spawn (centre_opening()).
    bool centre_map = false;
    bool elk_map = false; // elk_share of the gems behind elks or golems (world.cpp)
    float clear_share_now = policy::team_clear_share, ask_discount_now = policy::party_ask_discount; // routed by elk_map
    int opening_reach_now = 20, opening_rounds_now = 0; // policy's, or the routed map's (world.cpp)
    // A map in strategy.h watch_maps: the watch rules run (worker_skirmish()).
    bool watch_map = false;
    bool sweep_back = false; // A contested capture's sweep heads home.
    int watch_turns = 0; // Fight turns in a row with no enemy to strike from any tile in reach.
    float watch_score = 0; // About the count of such turns in the last watch_window turns.
    std::vector<bc::distance_field> altar_routes;
    TeamMemory shared;
    DraftPlan opening;
    // Whether the draft's plan holds this map's group and altar measures.
    // A centre map's opening (strategy.h): our own side first.
    bool centre_opening() const { return centre_map && int(round) < opening_rounds_now; }
    bool draft_measured() const {
        return opening.magic == DraftPlan::tag && opening.group_count == groups.size();
    }
    std::vector<float> roster_power;
    // Each roster slot's kit, camp damage multiplier and base damage per
    // round on camps.
    struct RosterKit {
        std::array<Spell, 2> spells;
        float camp_multiplier = 1, camp_rate = 0, focus = 0, health = 0;
        int range = 1;
    };
    std::vector<RosterKit> roster_kits;
    // Camp choice's workspace, per group its claimants' summed camp rates.
    std::vector<float> claim_rates;
    // Cluster runs (worker_choose_group()). Two cluster groups (gem or gold
    // clusters only) are linked when their nearest camp tiles lie within a
    // capture's time of each other. Per group: whether it is a cluster
    // group, and its links once made (cluster_links_of()), link_count of
    // them from link_first in cluster_links (-1: not made), nearest first,
    // the gap a walk once either group's walk field was there (walked), else
    // Chebyshev, checked at walk_cursor link_checked. And the rest of the run
    // being taken, its next member last.
    struct ClusterLink {
        int group = -1, apart = 0;
        bool walked = false;
    };
    std::vector<std::uint8_t> cluster_group;
    std::vector<std::int32_t> link_first;
    std::vector<std::uint16_t> link_count;
    std::vector<std::size_t> link_checked;
    std::vector<ClusterLink> cluster_links;
    std::vector<int> cluster_run;
    // behind_pressure() is worked out once a round.
    mutable int behind_round = -1;
    mutable float behind_value = 0;
    mutable float behind_gap = 0; // Their gems (tributed and carried) less ours, as behind_pressure() counts them.
    // The capture gold multiplier (Vesper's passive, Treasure).
    float gold_bonus = 1;
    // Turn ends our recall takes: five, three with a Home emblem.
    int recall_turns = 5;
    std::vector<bc::champion> enemy_roster;
    std::vector<int> enemy_respawn;
    // Each enemy's power at its design's full health (enemy_roster's order):
    // a body that respawns.
    std::vector<float> enemy_design_power;
    LifeSnapshot last_life;
    // Shards this champion has bought, by item type, for its build order.
    std::array<unsigned, static_cast<unsigned>(bc::item_type::count)> bought{};
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
    // be centred on (walls, holes and altars), and the two spawns.
    std::unique_ptr<std::uint16_t[]> tile_group;
    TileBits camp_tile, bush_tile, no_centre_tile, enemy_spawn_tile, own_spawn_tile;
    // Each open tile's walking region (tiles joined by steps; 0: blocked),
    // labelled once: terrain never changes. A crystal hop lands only in the
    // region of what it heads for (hop_lands_in_reach()).
    // Walking regions as the runs of open tiles along each row (initialise()):
    // row y's runs are [region_rows[y], region_rows[y + 1]), each from
    // region_start to region_end (exclusive) with its region's label. Only
    // equal labels are compared; 0 is a closed tile (walk_region_at()).
    std::unique_ptr<int[]> region_start, region_end, region_rows;
    std::unique_ptr<std::uint16_t[]> region_label;
    std::uint16_t walk_region_at(int x, int y) const;
    // The labelling, a row a step (begin_regions() in initialise(), then
    // region_step() in spare work; ensure_regions() before any hop reads it).
    int region_row = 0;
    bool regions_ready = false;
    std::uint64_t region_step_cost = 0; // The dearest row so far.
    void begin_regions();
    bool region_step();
    void ensure_regions();
    // Rows while the turn's fuel left covers twice the dearest so far: last in
    // a turn, in fuel it would not use (unused fuel does not carry over).
    void spare_region_steps();
    bool hop_lands_in_reach(vec2 landing) const;
    // Camps in view, refreshed by each observation (with the sleeping ones
    // when asleep_known, and the living ones); unseen ward placements.
    TileBits camp_in_view, camp_asleep, camp_living, unseen_tile;
    TileBits path_reached; // measure_paths()'s search: the tiles it reached.
    bool asleep_known = false;
    // Per group, with each observation: a camp of it in view, and one of us
    // (or an ally in view) standing on it. The camp tiles in view, listed.
    std::vector<std::uint8_t> group_in_view, group_held;
    // Per group, the round from which observe() looks at it out of view and
    // unheld with no records (held: always; cooling: its respawn; capture:
    // past its timer; else never), kept by note_group_due().
    std::vector<std::int32_t> group_due;
    void note_group_due(std::size_t group);
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
    // cross or land on; our spawn (the engine builds a new mask per call,
    // about 600 fuel); and the enemy spawn's tiles.
    bc::bitboard camp_tiles{bc::uninitialized}, dash_terrain{bc::uninitialized}, own_spawn{bc::uninitialized};
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
    // The walk from a tile to a group: its own field's when built, else a
    // lower bound (the straight line, the landmark fields).
    int walk_bound(vec2 from, int group) const;
    // The lower bound itself, from the landmark fields' values at `from`.
    int landmark_bound(int group, vec2 from, int home_here, int altar_here) const;
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
    // The nearest unit to threat k's camp, and in threat_ties[k] the units
    // there, worked out once per threat_revision.
    int threat_target(std::size_t k) const;
    void observe_fight();
    void publish(unsigned claim);
    float incoming(vec2 tile, bool include_camps = true) const;
    float incoming_enemies(vec2 tile) const;
    // incoming() at many tiles with nothing changing between the calls:
    // begin_scan() starts a scan, and incoming_scan(tile) gives incoming(tile)
    // with each camp's tile-independent tests made once for the scan.
    void begin_scan();
    float incoming_scan(vec2 tile) const;
    mutable std::vector<std::uint32_t> scan_stamp;
    mutable std::vector<int> scan_near;
    mutable std::vector<float> scan_hit;
    std::uint32_t scan_serial = 0;
    // The damage enemy champions deal a champion on `tile` that leaves now,
    // a tile a round, until it is out of every one's reach, before its
    // mitigation (exit_lanes): each ready burst that reaches it next round
    // lands once (a second the round after: one cast a turn); each enemy
    // hits once a round while it is within that enemy's reach (one stunned
    // now starts when its stun ends), a ready dash or reset that brings a
    // strike within reach adding one; a ready stun on it (none when
    // `immune`) holds it a round, a hit more from each enemy that reaches
    // it. An enemy on `without` is left out. With an enemy to leave, each
    // awake camp whose nearest champion it would be (a tie shares the hits)
    // hits it once a round until it is out of the camp's reach, after our
    // camp mitigation. And one round of the hits of the enemies that hit it
    // at all.
    struct ExitDamage {
        float damage = 0, round = 0;
    };
    ExitDamage exit_damage(vec2 tile, bool immune, vec2 without = {-1, -1}) const;
    // Its camps' part alone (incoming()'s threats), and the whole given
    // that part.
    float exit_camps(vec2 tile) const;
    ExitDamage exit_damage(vec2 tile, bool immune, vec2 without, float camps) const;
    // exit_lanes, made again when stale.
    const ExitLanes &exit_packed() const;
    // Exit damage and the bag margin (a round of those hits per exit_bag_gems
    // gems carried), after `mitigation`; both 0 when the most the enemies
    // could deal with that margin stays under `floor` (a lower bound of
    // what they are weighed against: the answer is the same).
    struct ExitRisk {
        float damage = 0, margin = 0;
    };
    ExitRisk exit_risk(vec2 tile, float floor, float mitigation, float camp_share, unsigned carried, bool immune) const;
    // Our health, shield and a ready shield spell's (cast as we walk), given
    // our shield and slot readiness, or read here.
    float exit_pool(float shield, const std::array<bool, 2> &is_ready) const;
    float exit_pool() const;
    // A step or landing onto a tile whose exit damage and bag margin reach
    // our pool and are worse than here's: the micro refuses it. The pool and
    // the damage here are read on first need.
    struct ExitCheck {
        const Bot &bot;
        float pool = -1, here = 0;
        bool lethal(vec2 tile);
    };
    // The sides on a tile, as XSquare's micro weighs them: each unit that
    // can act on it by next turn (its attack or spell reach and a move; none
    // while a stun holds it), undiscounted, at its power (unit_power(): full
    // health on its spawn, where it heals; no burst while stunned). Theirs:
    // enemies in view and out of sight (from their possible tile nearest
    // us), not channelling; ours: we wherever we stand and allies in view,
    // not channelling. It replaced friendly_power() and enemy_power(), which
    // discounted units by their distance (0.9 / (1 + 0.4 arrival), / (1 +
    // 0.3 arrival), none past 6).
    bool acts_on(const ObservedUnit &u, vec2 tile) const;
    float unit_power_of(const ObservedUnit &u, bool full) const;
    std::pair<float, float> sides_at(vec2 tile) const;
    // Our own power alone.
    float own_power() const;
    float respawn_threat(vec2 at) const;
    float dps(const CampGroup &g) const;
    // The walk out of the group's reach under its fire (camp choice's), and
    // whether the health we have clears it alone and pays that walk.
    float visit_cost(const CampGroup &g) const;
    bool visit_clears(const CampGroup &g) const;
    // navigation.cpp
    // Of equally short steps, the one nearest `toward` in a straight line.
    bool move_to(const bc::bitboard &goals, vec2 toward, bool urgent = false, bool use_dash = true,
                 int engaged_group = -1);
    // Teleport crystals. From our spawn, each group's and altar's walk with
    // one hop (unreachable when a hop saves nothing), built once from
    // terrain (prepare_hops()) for the choices made there.
    std::vector<int> group_hops, altar_hops;
    // Built, and whether any group or altar has a shorter hop route from
    // spawn (crystals are bought only then).
    bool hops_ready = false, hops_useful = false;
    // The hop on the way to `toward` (as move_to() is given it), planned
    // once per goal by plan_hop() at `round`: where it lands (none: x -1),
    // the walk to its take-off and the whole way's rounds.
    struct HopPlan {
        vec2 toward{-1, -1}, landing{-1, -1};
        int round = 0, takeoff_walk = 0, rounds = bc::distance_field::unreachable;
    } hop_plan;
    bool walking_to_hop = false;
    // Our income so far (buy_equipment() adds what each turn brought): gold
    // and gems gained, and what we held after the last turn's shopping.
    float earned_gold = 0, earned_gems = 0;
    unsigned gold_seen = 0, gems_seen = 0;
    // A gold's value in gems now (policy::early_gold_value to late_gold_value).
    float gold_value_now() const;
    // Our build's power gain per gold against the roster's average
    // (build_gain_per_gold()): how much more a gold makes us than a teammate.
    float gold_type_weight() const;
    // The rounds a hop must save to pay for its crystal (infinite before any
    // income).
    float hop_threshold() const;
    // Field buffers for plan_hop() and prepare_hops(), rows padded for SIMD
    // (allocated by hop_buffers()).
    std::unique_ptr<std::uint16_t[]> hop_goal, hop_from, hop_rows, hop_near;
    // plan_hop()'s goal side for the last few goal sets: the walk to the goals
    // (G) and its window minimum over the landing regions (M, rows padded to
    // a multiple of 8), both from the goals, the regions and static terrain.
    struct HopGoals {
        bc::bitboard goals;
        std::uint16_t allowed[9];
        int kinds = -1; // -1: empty
        bool near_ready = false; // M is built on the goals' second plan.
        std::unique_ptr<std::uint16_t[]> walk, near;
    };
    std::array<HopGoals, 4> hop_cache;
    unsigned hop_cache_next = 0;
    bc::bitboard hop_cache_diff;
    void hop_buffers();
    bc::distance_field hop_goal_field, hop_from_field;
    void prepare_hops();
    bool carrying_crystal() const;
    // The rounds to `toward` the plan has left (unreachable without one).
    int hop_rounds(vec2 toward) const;
    // The best hop from here to `goals` against walking there.
    void plan_hop(const bc::bitboard &goals, float threshold);
    // move_to()'s part: -1 to walk on, else whether it acted (walked to the
    // take-off or started the hop).
    int follow_hop(const bc::bitboard &goals, vec2 toward, int walk, bool use_dash, int engaged_group);
    bool take_hop();
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
    // A kill, as XSquare's sage counts one: more than any damage this turn
    // (all the health of the enemies in view), so the most kills come
    // first and the most damage second.
    float kill_value() const;
    // A stun's worth (one round, the enemy's next turn skipped): the damage
    // that turn would have dealt one of ours it reaches at most (capped at
    // that one's health), and the window, our teammates still to act this
    // round hitting it where it stands. stun_value() adds the two for an
    // enemy left with `left` health: a kill when the window covers it.
    float stun_prevented(const ObservedUnit &enemy) const;
    float stun_follow_up(const ObservedUnit &enemy) const;
    float stun_value(const ObservedUnit &enemy, float left) const;
    // A shield's worth on the same scale: the damage it absorbs over its
    // turns (`danger` a turn) beyond what the shield we hold (`held`)
    // absorbs, as a new one replaces it; and our death when it turns a
    // lethal next turn survivable. It replaced the absorbed damage x 0.85,
    // x 2 below four turns of danger, x 0.2 while shielded.
    float shield_value(float shield, float danger, int turns, float held) const;
    // Our own death, the same: one kill, theirs; carrying gems, more than any
    // kill of ours, as the killer takes the bag (a trade of our death for a
    // kill, against Ren's retaliation, is even only with nothing carried).
    float death_cost() const { return kill_value() * (gems > 0 ? 2.0f : 1.0f); }
    bool attack_combo(bool escaping);
    bool impact_dash(bool fighting_champions, int farm_group);
    bool advance_attack(vec2 victim, int stand = 0);
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
    bool outranks(const ObservedUnit &ally, bool arriving, const CampGroup &g) const;
    // An ally whose published code asks for help in a fight.
    bool reports_fight(const ObservedUnit &ally) const;
    // The round a capture we hold pays (its reward's capture phase).
    int capture_payout(const CampGroup &g) const;
    // Whether a teammate near a capture holds it from the start instead of us.
    bool capture_deferred(const CampGroup &g) const;
    void note_altar_locks();
    bool lock_wanted(std::size_t altar) const;
    void place_lock();
    // This turn's champion is hurt (the health retreat's test, or already
    // retreating): the fight plays XSquare's hurt mode, then the retreat walks
    // it home when no fight acts.
    bool hurt_turn = false;
    bool worker_pursue();
    bool home_after_capture(const CampGroup &g) const;
    // How far behind in the gem race we are, 0 (even or ahead) to 1: the gap
    // against what the leader still needs to win.
    float behind_pressure() const;
    // Cluster runs (worker_choose_group()): which groups are cluster groups
    // (once), and a cluster group's links, made on its first need and their
    // walks checked as walk fields come. The span lasts until the next
    // group's links are made.
    void link_clusters();
    std::span<const ClusterLink> cluster_links_of(int g);
    // The walk between two groups' nearest camp tiles, from either one's walk
    // field (walking is symmetric): one of them must be built.
    int cluster_walk(int a, int b) const;
    // A run member is open on arrival: no teammate's claim on it (`claimed`),
    // no enemy seen near it lately, and alive or back by then.
    bool cluster_open(int group, int arrival, bool claimed) const;
    // A teammate claims the group (mate_claim()); an enemy was seen
    // near it within policy::worker_sighting_memory rounds.
    bool teammate_claims(int group) const;
    bool enemy_near_lately(int group) const;
    // The run's next member is open by the time we walk there from here.
    bool run_continues() const;
    float basic_damage() const;
    // Our damage's factor from Heart of Ruin's passive (1 without it), as
    // ObservedUnit applies it to theirs.
    float damage_bonus() const;
    void track_enemy_respawns();
    bool camp_dormant(const ObservedUnit &c) const;
    int claimed_by(const ObservedUnit &ally) const;
    // A teammate's camp claim: the group a living teammate's plain claim
    // names, else -1 (every reader of claims uses this one rule).
    int mate_claim(unsigned s) const;
    bc::bitboard siege_tiles(const CampGroup &g, float reach, bool assume_alive) const;
    void prepare_sieges();
    bool ally_draws_attacks(const CampGroup &g) const;
    bool worker_skirmish();
    // The fight decision against the hidden record alone (nothing in view).
    bool confront_hidden();
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
    // Banking blocked by camps: cut through the cheap ones nearer the altar.
    bool corridor_attack(vec2 altar);
    void note_activity(Activity activity);
    // safety.cpp: danger from enemy champions, remembered sightings, and
    // stepping away.
    bool enemy_threatens(const ObservedUnit &enemy, vec2 tile) const { return enemy_threatens(enemy, enemy.position, tile); }
    // As if the enemy stood on `at`.
    bool enemy_threatens(const ObservedUnit &enemy, vec2 at, vec2 tile) const;
    void track_ghosts();
    // A hit nothing in view could have struck makes the hidden record.
    void note_hidden_hit(float lost);
    // The most the living camps with our tile in reach strike for in a
    // round, as the records stand.
    float camp_reach_damage() const;
    // Drops the hidden record from this turn's ghosts.
    void drop_hidden();
    void worker_track_sightings();
    std::pair<int, int> worker_danger(vec2 tile) const;
    // The tile a step away from danger takes (our own: none).
    vec2 step_away_tile();
    // A melee champion's strike, from where it stands, before a step away.
    void strike_before_step();
    // Run or fight while retreating home under fire.
    Chase::Mode chase_mode();
    std::uint32_t threat_bits() const;
    bool duel_won(std::uint32_t threats, float pool);
    bool race(vec2 goal, int arrive, bool use_dash, float pool);

    void take_turn();

  public:
    void play_turn();
};
