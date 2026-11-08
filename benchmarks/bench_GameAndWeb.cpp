/*
 * The game and web pages in one main loop, in real time: what the players and the readers wait for,
 * and how steady the world's tick is, while the web is flooded. Not a microbenchmark: run alone,
 * ./benchmarks "[gameweb]", on an idle machine with an optimized build; it prints a table.
 *
 *  - the game: Players players, each sending MovesPerSecond small packets (a few microseconds each),
 *    the world updated at a tick that takes WorldWork
 *  - the web: ReadsPerSecond requests from ReaderAddresses addresses, each a query on a worker
 *  - then a flood: addresses sending a type without an AntiDOS limit as fast as they can (the worst
 *    case: no 429, no block). Either requests with a query (FloodAddresses, the workers are what they
 *    take), or requests answered from memory (CheapFloodAddresses, CheapCost of the main thread each,
 *    what they take is the update)
 *
 * Three ways to run them in one loop: the world every update (an update every tick), the world
 * at its own tick with every session in one class, and the world at its own tick with the game a class
 * of its own, served first (docs/29-server-core.md, 3.1.1 and 3.1.2).
 */

#include "rooftree_benchmark.h"

#include "FixedTicker.h"
#include "SessionMgr.h"
#include "UpdateLoop.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace Rooftree::Server;

namespace
{
constexpr int Players = 1000;
constexpr int MovesPerSecond = 10;
constexpr auto MoveCost = std::chrono::microseconds(2);
constexpr auto WorldWork = 10ms;
constexpr int ReadsPerSecond = 500;
constexpr int ReaderAddresses = 200;
constexpr auto QueryTime = std::chrono::microseconds(300);
constexpr int FloodAddresses = 8;
constexpr int FloodPerAddressPerMs = 20;
constexpr int CheapFloodAddresses = 64;
constexpr auto CheapCost = std::chrono::microseconds(5);
constexpr auto PhaseTime = 6s;

void Spin(std::chrono::microseconds duration)
{
    TimePoint until = TimePoint::clock::now() + duration;
    while (TimePoint::clock::now() < until) { }
}

uint32 MicrosSince(TimePoint since)
{
    return uint32(std::chrono::duration_cast<std::chrono::microseconds>(TimePoint::clock::now() - since).count());
}

// Main thread only: messages are processed and works finish there
struct Measures
{
    std::vector<uint32> Game;       // us from enqueued to processed
    std::vector<uint32> Web;        // us from enqueued to its query done, the normal readers'
    std::vector<uint32> TickGaps;   // us between two world ticks
    uint32 FloodDone = 0;
    std::atomic<uint32> Rejected = 0;
    uint32 Dropped = 0;
    uint32 MaxUpdate = 0;           // us
};

class MoveMessage final : public Message
{
public:
    MoveMessage(MessageType const& type, std::string key, Measures& measures)
        : Message(type, std::move(key), boost::asio::ip::make_address("10.1.0.1")), _since(TimePoint::clock::now()), _measures(measures) { }

    void Process(Session&) override
    {
        Spin(MoveCost);
        _measures.Game.push_back(MicrosSince(_since));
    }

    void Reject(RejectReason) override { ++_measures.Rejected; }

private:
    TimePoint _since;
    Measures& _measures;
};

// The readers' query
Rooftree::Task<void> ReadAsCoroutine(Session& session, TimePoint since, bool flood, Measures& measures)
{
    co_await session.OnWorker([] { std::this_thread::sleep_for(QueryTime); });
    if (flood)
        ++measures.FloodDone;
    else
        measures.Web.push_back(MicrosSince(since));
}

class ReadMessage final : public Message
{
public:
    ReadMessage(MessageType const& type, std::string key, boost::asio::ip::address address, bool flood, Measures& measures, bool cheap = false)
        : Message(type, std::move(key), std::move(address)), _since(TimePoint::clock::now()), _flood(flood), _cheap(cheap), _measures(measures) { }

    void Process(Session& session) override
    {
        if (_cheap)
        {
            Spin(CheapCost);
            ++_measures.FloodDone;
            return;
        }

        session.Spawn(ReadAsCoroutine(session, _since, _flood, _measures));
    }

    void Reject(RejectReason) override { ++_measures.Rejected; }

private:
    TimePoint _since;
    bool _flood;
    bool _cheap;
    Measures& _measures;
};

class SimSessionMgr final : public SessionMgr
{
public:
    SimSessionMgr() : SessionMgr(4) { }

protected:
    std::unique_ptr<Session> CreateSession(Message const& firstMessage) override
    {
        return std::make_unique<Session>(*this, firstMessage.GetSessionKey(), firstMessage.GetRemoteAddress());
    }
};

struct Mode
{
    char const* Name;
    bool GameClass;         // the game's sessions a class of their own, served first
    bool SeparateTick;      // the loop every 2 ms, the world at its tick; else an update per tick
    uint32 WebBudget = 2000;  // the web's (default class's) MaxMessagesPerUpdate
};

struct PhaseResult
{
    uint32 GameP50, GameP99, WebP50, WebP99, TickP99, MaxUpdate, Dropped;
    uint32 FloodPerSecond;
};

uint32 Percentile(std::vector<uint32>& values, double p)
{
    if (values.empty())
        return 0;
    std::ranges::sort(values);
    return values[std::min(values.size() - 1, std::size_t(double(values.size()) * p))];
}

enum class Flood { None, Queries, Cheap };

PhaseResult RunPhase(Mode const& mode, Flood flood)
{
    Measures measures;
    {
        SimSessionMgr mgr;
        MessageType const& move = mgr.GetMessageTypes().Register("game.move", mode.GameClass ? "game" : "");
        MessageType const& read = mgr.GetMessageTypes().Register("web.read");

        // blog.yaml.dist's
        SessionMgrSettings settings{ .MaxMessagesPerSessionPerUpdate = 100, .MaxMessagesPerUpdate = mode.WebBudget, .MaxQueuedMessagesPerSession = 64,
            .MaxPendingWorkPerSession = 4 };
        if (mode.GameClass)
            settings.Classes["game"] = { .Priority = 0, .MaxMessagesPerSessionPerUpdate = 100, .MaxMessagesPerUpdate = 20000,
                .MaxQueuedMessagesPerSession = 200, .MaxPendingWorkPerSession = 4, .Block = BlockScope::Session };
        mgr.LoadSettings(settings, {});

        std::atomic<bool> stop = false;
        std::vector<std::thread> producers;
        // The players: every millisecond the moves of that millisecond
        producers.emplace_back([&]
        {
            TimePoint next = TimePoint::clock::now();
            uint64 sent = 0;
            for (uint64 ms = 0; !stop; ++ms)
            {
                uint64 due = (ms + 1) * Players * MovesPerSecond / 1000;
                for (; sent < due; ++sent)
                    mgr.Enqueue(std::make_unique<MoveMessage>(move, "player:" + std::to_string(sent % Players), measures));
                next += 1ms;
                std::this_thread::sleep_until(next);
            }
        });
        producers.emplace_back([&]
        {
            TimePoint next = TimePoint::clock::now();
            uint64 sent = 0;
            for (uint64 ms = 0; !stop; ++ms)
            {
                uint64 due = (ms + 1) * ReadsPerSecond / 1000;
                for (; sent < due; ++sent)
                {
                    uint32 reader = uint32(sent % ReaderAddresses);
                    mgr.Enqueue(std::make_unique<ReadMessage>(read, "ip:10.2.0." + std::to_string(reader),
                        boost::asio::ip::make_address_v4(0x0A020000 + reader), false, measures));
                }
                next += 1ms;
                std::this_thread::sleep_until(next);
            }
        });
        if (flood != Flood::None)
            producers.emplace_back([&]
            {
                bool cheap = flood == Flood::Cheap;
                int addresses = cheap ? CheapFloodAddresses : FloodAddresses;
                TimePoint next = TimePoint::clock::now();
                while (!stop)
                {
                    for (int address = 0; address < addresses; ++address)
                        for (int i = 0; i < FloodPerAddressPerMs; ++i)
                            mgr.Enqueue(std::make_unique<ReadMessage>(read, "ip:10.3.0." + std::to_string(address),
                                boost::asio::ip::make_address_v4(0x0A030000 + address), true, measures, cheap));
                    next += 1ms;
                    std::this_thread::sleep_until(next);
                }
            });

        UpdateLoop loop(mode.SeparateTick ? 2ms : 50ms);
        loop.SetStatsInterval(0ms);
        FixedTicker ticker;
        ticker.SetInterval(50ms);
        TimePoint lastTick;
        auto worldTick = [&](uint32)
        {
            TimePoint now = TimePoint::clock::now();
            if (lastTick != TimePoint())
                measures.TickGaps.push_back(uint32(std::chrono::duration_cast<std::chrono::microseconds>(now - lastTick).count()));
            lastTick = now;
            Spin(WorldWork);
        };

        std::thread stopper([&] { std::this_thread::sleep_for(PhaseTime); loop.Stop(); });
        loop.Run([&](uint32 diff)
        {
            TimePoint start = TimePoint::clock::now();
            mgr.Update(diff);
            if (mode.SeparateTick)
                ticker.Update(diff, worldTick);
            else
                worldTick(diff);
            measures.MaxUpdate = std::max(measures.MaxUpdate, MicrosSince(start));
        });
        stopper.join();
        stop = true;
        for (std::thread& producer : producers)
            producer.join();
        measures.Dropped = ticker.TakeStats().Dropped;
    }

    return { .GameP50 = Percentile(measures.Game, 0.5), .GameP99 = Percentile(measures.Game, 0.99),
        .WebP50 = Percentile(measures.Web, 0.5), .WebP99 = Percentile(measures.Web, 0.99),
        .TickP99 = Percentile(measures.TickGaps, 0.99), .MaxUpdate = measures.MaxUpdate, .Dropped = measures.Dropped,
        .FloodPerSecond = uint32(measures.FloodDone / std::chrono::duration_cast<Seconds>(PhaseTime).count()) };
}
}

TEST_CASE("The game and web pages in one loop, the web flooded", "[.][gameweb]")
{
    Mode const modes[] = {
        { "world every update, 50 ms", false, false },
        { "tick apart, one class", false, true },
        { "tick apart, game class first", true, true },
        { "game first, web 500 an update", true, true, 500 },
    };

    std::printf("\n%-32s %-8s %16s %16s %10s %10s %8s %10s\n", "mode", "flood", "game p50/p99 ms", "web p50/p99 ms",
        "tick p99", "max upd", "dropped", "flood/s");
    for (Mode const& mode : modes)
        for (Flood flood : { Flood::None, Flood::Queries, Flood::Cheap })
        {
            PhaseResult r = RunPhase(mode, flood);
            std::printf("%-32s %-8s %7.1f/%-8.1f %7.1f/%-8.1f %8.1fms %8.1fms %8u %10u\n", mode.Name,
                flood == Flood::None ? "none" : flood == Flood::Queries ? "queries" : "cheap",
                r.GameP50 / 1000.0, r.GameP99 / 1000.0, r.WebP50 / 1000.0, r.WebP99 / 1000.0, r.TickP99 / 1000.0,
                r.MaxUpdate / 1000.0, r.Dropped, r.FloodPerSecond);
            std::fflush(stdout);
        }
    SUCCEED();
}
