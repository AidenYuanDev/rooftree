/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The world thread's state: the sessions, the coins on the field, the
 * combat between the players and the timed world events. Everything here runs on the world thread (main.cpp's update loop) except
 * AddSession, StopNow and RequestReload, which other threads use to hand work to it.
 *
 * Each Update(diff):
 *   1. reload the config if SIGHUP asked for it (nothing else runs, so cached settings need no lock)
 *   2. add the sessions the network threads logged in, update every session (run its messages)
 *   3. move the players, collect coins, work out who sees whom (combat/Visibility.h), run the combat
 *      (casts, auras) and send what happened in it, run the TaskScheduler and the EventMap
 *   4. send every client the new state
 *
 * A player whose connection went stays in the world for Arena.LingerTime, standing, still to be hit: a
 * login in that time takes it over as it was (hurt, dead, on cooldown), so a relog heals or revives
 * nothing. After it, it is saved and goes.
 *
 * A client hears only of what its player sees: the State lists the players within
 * Arena.World.ViewDistance (Arena.World.MaxVisible of them at most, the nearest) and the coins in that
 * distance, the combat log the events whose caster or target it sees. System messages still go to all.
 */

#ifndef ARENA_WORLD_H
#define ARENA_WORLD_H

#include "CombatMap.h"
#include "Define.h"
#include "Duration.h"
#include "EventMap.h"
#include "LockedQueue.h"
#include "TaskScheduler.h"
#include "Visibility.h"
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ArenaSession;
class Config;
class Player;
class ServerPacket;

// Arena.* of the config, reloadable
struct WorldSettings
{
    float Width = 1000.0f;
    float Height = 700.0f;
    float PlayerSpeed = 220.0f;             // per second
    float ViewDistance = 600.0f;            // a player sees this far
    uint32 MaxVisible = 30;                 // players in one State at most, itself too; 0 for no limit
    uint32 MaxCoins = 15;
    Milliseconds CoinSpawnInterval = 1s;
    Milliseconds GoldRushInterval = 60s;
    Milliseconds GoldRushDuration = 10s;
    uint32 GoldRushCoinValue = 3;
    Milliseconds SaveInterval = 30s;
    Milliseconds IdleTimeout = 30s;
    uint32 MaxChatLength = 200;             // characters
    Milliseconds ChatInterval = 500ms;      // between two lines of one player
    Milliseconds RespawnTime = 5s;
    Milliseconds LingerTime = 30s;          // a disconnected player stays in the world
    std::string Motd;
};

class World
{
public:
    static World* instance();

    World(World const&) = delete;
    World& operator=(World const&) = delete;

    void LoadConfigSettings(Config const& config);
    WorldSettings const& GetSettings() const { return _settings; }

    // Schedules the world events, once before the first Update
    void SetInitialWorldSettings();

    CombatMap& GetCombatMap() { return _combat; }

    // Any thread: queued, the session is added in the next Update
    void AddSession(std::shared_ptr<ArenaSession> session);

    void Update(uint32 diff);

    // Any thread
    static void StopNow() { _stopEvent = true; }
    static bool IsStopped() { return _stopEvent; }
    void RequestReload() { _reloadRequested = true; }

    // Shutdown, after the update loop ended
    void SaveAllPlayersDirect();
    void KickAll(std::string_view reason);  // with 1001 Going Away
    bool AreAllSocketsClosed() const;
    void ClearSessions();

    // A SystemMessage to everyone in game
    void SendSystemMessage(std::string_view text);
    // unreliable: may be lost over KCP, for what the next one replaces
    void SendGlobalMessage(ServerPacket const& packet, bool unreliable = false);

private:
    World() = default;

    void AddQueuedSessions();
    void UpdateSessions(uint32 diff);
    // Its player, if any, lingers
    void RemoveSession(ArenaSession& session);
    void UpdateLingering(uint32 diff);
    // Saved and gone
    void RemovePlayer(Player& player);
    // In game or lingering
    Player* FindPlayer(uint32 id) const;
    void UpdatePlayers(uint32 diff);
    void UpdateVisibility();
    void UpdateCombat(uint32 diff);
    void OnDeath(uint32 victimId, uint32 killerId);
    void Respawn(uint32 accountId);
    void UpdateEvents(uint32 diff);
    void SpawnCoin();
    void SavePlayers();
    void SendState();
    void LogUpdateStats();

    struct Coin
    {
        uint32 Id;
        float X;
        float Y;
        uint32 Value;
    };

    static inline std::atomic<bool> _stopEvent = false;
    std::atomic<bool> _reloadRequested = false;

    WorldSettings _settings;

    LockedQueue<std::shared_ptr<ArenaSession>> _addSessionQueue;
    std::unordered_map<uint32, std::shared_ptr<ArenaSession>> _sessions;   // by account id

    struct Lingering
    {
        std::unique_ptr<Player> Character;
        Milliseconds Left;
    };
    std::unordered_map<uint32, Lingering> _lingering;                       // by account id

    std::vector<Coin> _coins;
    uint32 _nextCoinId = 1;
    bool _goldRush = false;

    CombatMap _combat;
    Visibility _visibility;     // as of this update's movement

    // Since the last LogUpdateStats
    struct UpdateStats
    {
        uint32 Updates = 0;
        std::chrono::microseconds Total = 0us;
        std::chrono::microseconds Max = 0us;
        uint64 CombatEvents = 0;
        uint64 States = 0;
        uint64 StateBytes = 0;
        uint64 StatesReliable = 0;      // too large for one datagram: KCP sends them reliably
        uint64 StatePlayers = 0;
    } _updateStats;

    TaskScheduler _scheduler;   // coin spawns, periodic saves
    EventMap _events;           // gold rush start/end
};

#define sWorld World::instance()

#endif // ARENA_WORLD_H
