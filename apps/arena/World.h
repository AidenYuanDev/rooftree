/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The world thread's state: the sessions, the coins on the field and the
 * timed world events. Everything here runs on the world thread (main.cpp's update loop) except
 * AddSession, StopNow and RequestReload, which other threads use to hand work to it.
 *
 * Each Update(diff):
 *   1. reload the config if SIGHUP asked for it (nothing else runs, so cached settings need no lock)
 *   2. add the sessions the network threads logged in, update every session (run its messages)
 *   3. move the players, collect coins, run the TaskScheduler and the EventMap
 *   4. send every client the new state
 */

#ifndef ARENA_WORLD_H
#define ARENA_WORLD_H

#include "Define.h"
#include "Duration.h"
#include "EventMap.h"
#include "LockedQueue.h"
#include "TaskScheduler.h"
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ArenaSession;
class Config;
class ServerPacket;

// Arena.* of the config, reloadable
struct WorldSettings
{
    float Width = 1000.0f;
    float Height = 700.0f;
    float PlayerSpeed = 220.0f;             // per second
    uint32 MaxCoins = 15;
    Milliseconds CoinSpawnInterval = 1s;
    Milliseconds GoldRushInterval = 60s;
    Milliseconds GoldRushDuration = 10s;
    uint32 GoldRushCoinValue = 3;
    Milliseconds SaveInterval = 30s;
    Milliseconds IdleTimeout = 30s;
    uint32 MaxChatLength = 200;             // characters
    Milliseconds ChatInterval = 500ms;      // between two lines of one player
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
    void RemoveSession(ArenaSession& session);
    void UpdatePlayers(uint32 diff);
    void UpdateEvents(uint32 diff);
    void SpawnCoin();
    void SavePlayers();
    void SendState();

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

    std::vector<Coin> _coins;
    uint32 _nextCoinId = 1;
    bool _goldRush = false;

    TaskScheduler _scheduler;   // coin spawns, periodic saves
    EventMap _events;           // gold rush start/end
};

#define sWorld World::instance()

#endif // ARENA_WORLD_H
