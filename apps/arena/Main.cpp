/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * arena: a small multiplayer browser game on top of Rooftree.
 *
 *   main thread         World::Update loop (WorldUpdateLoop), everything game related
 *   io thread           acceptors of both services, signals, HTTP session cleanup timer
 *   HTTP network        AuthService's NetworkThreads, plus its worker ThreadPool (Argon2, sync queries)
 *   WebSocket network   ArenaSocketMgr's NetworkThreads, parse messages into ArenaSession queues
 *   KCP network         ArenaKcpServer's thread, the same for native clients over UDP (Arena.Kcp)
 *   database            DatabaseWorkerPool's async worker(s)
 *
 * Signals: SIGINT/SIGTERM stop the server (players are saved and get a close frame), SIGHUP reloads
 * the config at the start of the next world update.
 *
 * Try:  ./arena  then open http://127.0.0.1:8090/ (needs Database.Arena in arena.yaml)
 */

#include "ArenaDatabase.h"
#include "ArenaKcp.h"
#include "ArenaSocket.h"
#include "AuthService.h"
#include "Config.h"
#include "DatabaseConfig.h"
#include "IoContext.h"
#include "Log.h"
#include "Timer.h"
#include "World.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/signal_set.hpp>
#include <fstream>
#include <sstream>
#include <thread>

namespace
{
using SignalSet = boost::asio::basic_signal_set<Rooftree::Asio::IoContextExecutor>;

// Every statement of the schema file, they are all idempotent
bool ApplySchema(std::string const& file)
{
    std::ifstream in(file);
    if (!in)
    {
        RT_LOG_ERROR("arena", "Can't open {}, check Arena.SqlFile", file);
        return false;
    }

    std::ostringstream content;
    content << in.rdbuf();

    // Statements end with ';', comments are whole lines starting with --. Enough for our own file
    std::string statement;
    std::istringstream lines(std::move(content).str());
    for (std::string line; std::getline(lines, line);)
    {
        if (line.starts_with("--"))
            continue;

        statement += line;
        statement += '\n';
        if (line.find(';') == std::string::npos)
            continue;

        ArenaDatabase.DirectExecute(statement.c_str());
        statement.clear();
    }

    return true;
}

bool StartDB(Config const& config)
{
    std::optional<DatabaseConfig> database = config.Get<DatabaseConfig>("Database.Arena");
    if (!database)
    {
        RT_LOG_ERROR("arena", "Database.Arena is not set or invalid, it needs at least Host, User and Name");
        return false;
    }


    ArenaDatabase.SetConnectionInfo(database->Connection, database->WorkerThreads, database->SynchThreads);
    if (ArenaDatabase.Open())
    {
        RT_LOG_ERROR("arena", "Could not connect to the arena database");
        return false;
    }

    // Statements are prepared against the tables, they have to exist first
    if (!ApplySchema(config.Get<std::string>("Arena.SqlFile", "arena.sql")) || !ArenaDatabase.PrepareStatements())
    {
        ArenaDatabase.Close();
        return false;
    }

    return true;
}

void StopDB()
{
    ArenaDatabase.Close();
}

// Update as soon as tick passed, with the real time since the last update
void WorldUpdateLoop(uint32 minUpdateDiff)
{
    uint32 realPrevTime = getMSTime();
    while (!World::IsStopped())
    {
        uint32 realCurrTime = getMSTime();
        uint32 diff = getMSTimeDiff(realPrevTime, realCurrTime);
        if (diff < minUpdateDiff)
        {
            std::this_thread::sleep_for(Milliseconds(minUpdateDiff - diff));
            continue;
        }

        sWorld->Update(diff);
        realPrevTime = realCurrTime;
    }
}

void WaitForReloadSignal(SignalSet& signals)
{
    signals.async_wait([&signals](boost::system::error_code const& error, int)
    {
        if (error)
            return;

        RT_LOG_INFO("arena", "SIGHUP: reloading the config");
        sWorld->RequestReload();
        WaitForReloadSignal(signals);
    });
}
}

int main(int argc, char** argv)
{
    std::string configFile = argc > 1 ? argv[1] : "arena.yaml";
    std::string configError;
    if (!sConfigMgr->LoadFile(configFile, configError))
    {
        fmt::print(stderr, "Could not load {}: {}\n", configFile, configError);
        return 1;
    }

    sLog->LoadFromConfig();

    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();
    if (!StartDB(*config))
        return 1;

    sWorld->LoadConfigSettings(*config);
    sWorld->SetInitialWorldSettings();

    Rooftree::Asio::IoContext ioContext;

    SignalSet stopSignals(ioContext, SIGINT, SIGTERM);
    stopSignals.async_wait([](boost::system::error_code const& error, int)
    {
        if (!error)
            World::StopNow();
    });

    SignalSet reloadSignals(ioContext, SIGHUP);
    WaitForReloadSignal(reloadSignals);

    std::string httpBindIp = config->Get<std::string>("Arena.Http.BindIP", "0.0.0.0");
    uint16 httpPort = config->Get<uint16>("Arena.Http.Port", 8090);
    std::string wsBindIp = config->Get<std::string>("Arena.WebSocket.BindIP", "0.0.0.0");
    uint16 wsPort = config->Get<uint16>("Arena.WebSocket.Port", 8091);
    bool kcpEnabled = config->Get<bool>("Arena.Kcp.Enabled", true);
    std::string kcpBindIp = config->Get<std::string>("Arena.Kcp.BindIP", "0.0.0.0");
    uint16 kcpPort = config->Get<uint16>("Arena.Kcp.Port", 8092);

    AuthSettings authSettings;
    authSettings.WebRoot = config->Get<std::string>("Arena.WebRoot", authSettings.WebRoot);
    authSettings.WebSocketPort = wsPort;
    authSettings.WebSocketUrl = config->Get<std::string>("Arena.WebSocket.PublicUrl", "");
    authSettings.KcpPort = kcpEnabled ? kcpPort : 0;
    authSettings.Argon2Iterations = config->Get<uint32>("Arena.Argon2.Iterations", authSettings.Argon2Iterations);
    authSettings.Argon2MemoryCost = config->Get<uint32>("Arena.Argon2.MemoryCost", authSettings.Argon2MemoryCost);

    if (!sAuthService.Start(ioContext, httpBindIp, httpPort, config->Get<int32>("Arena.Http.NetworkThreads", 1),
        std::move(authSettings), config->Get<uint32>("Arena.Http.WorkerThreads", 2)))
    {
        StopDB();
        return 1;
    }

    if (!sArenaSocketMgr.StartNetwork(ioContext, wsBindIp, wsPort, config->Get<int32>("Arena.WebSocket.NetworkThreads", 1)))
    {
        sAuthService.Stop();
        StopDB();
        return 1;
    }

    if (kcpEnabled)
    {
        Rooftree::Net::Kcp::Settings kcpSettings;
        kcpSettings.IdleTimeout = Milliseconds(config->Get<uint32>("Arena.Kcp.IdleTimeout", 15000));
        sArenaKcpServer.SetSettings(kcpSettings);
        sArenaKcpServer.SetMaxConnectionsPerAddress(config->Get<uint32>("Arena.Kcp.MaxConnectionsPerAddress", 0));
        if (!sArenaKcpServer.Start(kcpBindIp, kcpPort))
        {
            sArenaSocketMgr.StopNetwork();
            sAuthService.Stop();
            StopDB();
            return 1;
        }

        RT_LOG_INFO("arena", "KCP on udp://{}:{}", kcpBindIp, kcpPort);
    }

    // The acceptors and signal sets keep it busy, the guard keeps it running if they ever don't
    auto workGuard = boost::asio::make_work_guard(ioContext);
    std::thread ioThread([&ioContext] { ioContext.run(); });

    RT_LOG_INFO("arena", "Listening on http://{}:{}/ and ws://{}:{}/arena, Ctrl+C to stop", httpBindIp, httpPort, wsBindIp, wsPort);

    WorldUpdateLoop(config->Get<uint32>("Arena.TickInterval", 50));

    RT_LOG_INFO("arena", "Shutting down");

    sWorld->SaveAllPlayersDirect();
    sWorld->KickAll("server shutting down");

    // A socket stops being open in the same NetworkThread update that writes its close frame
    for (int i = 0; i < 100 && !sWorld->AreAllSocketsClosed(); ++i)
        std::this_thread::sleep_for(10ms);

    // The acceptors' io_context first, StopNetwork destroys the acceptors (see tests/test_Network.cpp)
    workGuard.reset();
    ioContext.stop();
    ioThread.join();

    sArenaKcpServer.Stop();
    sArenaSocketMgr.StopNetwork();
    sAuthService.Stop();
    sWorld->ClearSessions();

    StopDB();
    RT_LOG_INFO("arena", "Stopped");
    return 0;
}
