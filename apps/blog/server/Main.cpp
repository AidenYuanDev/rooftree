/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The blog's API server:
 *
 *   main          the config and the logs -> the database (BlogDatabase.h) -> the subcommands, or Serve
 *   Serve         the world and what it loads (BlogWorld::LoadFromDatabase) -> HTTP (and HTTPS) on their
 *                 network threads -> the signals, FreezeDetector and the console on the io thread -> the
 *                 update loop on this thread until SIGINT/SIGTERM -> everything stopped in reverse
 *
 *   blog [--config blog.yaml]                     serve
 *   blog [--config blog.yaml] --create-admin --username NAME --email ADDRESS [--display-name NAME] [--totp]
 *                                                 create the author's account (AdminCli.h)
 *   blog [--config blog.yaml] --console [--socket PATH]
 *                                                 the running server's console (Server.Console.Socket,
 *                                                 "help" lists the commands, ConsoleCommands.h)
 *
 * Serving on a terminal, it reads the same commands from it.
 *
 * kill -HUP reloads blog.yaml at the next update, and the HTTPS certificate right away.
 */

#include "AdminCli.h"
#include "BlogDatabase.h"
#include "BlogRoutes.h"
#include "BlogWebSocket.h"
#include "BlogWorld.h"
#include "Config.h"
#include "Console.h"
#include "ConsoleApi.h"
#include "ConsoleCommands.h"
#include "ConsoleSocket.h"
#include "IoContext.h"
#include "Log.h"
#include "UpdateLoop.h"
#include "WorldHttpService.h"
#include <print>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/signal_set.hpp>
#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <thread>

namespace
{
using Rooftree::Server::WorldHttpService;
using Rooftree::Server::WorldHttpSettings;
using SignalSet = boost::asio::basic_signal_set<Rooftree::Asio::IoContextExecutor>;

// Command line

struct Arguments
{
    std::string ConfigFile = "blog.yaml";
    bool Console = false;
    std::string ConsoleSocket;
    bool CreateAdmin = false;
    Blog::CreateAdminArguments Admin;
};

Optional<Arguments> ParseArguments(int argc, char** argv)
{
    Arguments arguments;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        auto value = [&]() -> Optional<std::string>
        {
            if (i + 1 >= argc)
                return {};
            return std::string(argv[++i]);
        };

        Optional<std::string> v;
        if (arg == "--create-admin")
            arguments.CreateAdmin = true;
        else if (arg == "--totp")
            arguments.Admin.Totp = true;
        else if (arg == "--console")
            arguments.Console = true;
        else if (arg == "--socket" && (v = value()))
            arguments.ConsoleSocket = *v;
        else if (arg == "--config" && (v = value()))
            arguments.ConfigFile = *v;
        else if (arg == "--username" && (v = value()))
            arguments.Admin.Username = *v;
        else if (arg == "--email" && (v = value()))
            arguments.Admin.Email = *v;
        else if (arg == "--display-name" && (v = value()))
            arguments.Admin.DisplayName = *v;
        else
        {
            std::print(stderr, "Unknown or incomplete argument {}\n", arg);
            return {};
        }
    }

    return arguments;
}

// HTTP

// The host itself: the rebuild's prerendering reads every page through the API on it
std::vector<std::string> const LoopbackAddresses = { "127.0.0.1", "::1" };

std::vector<boost::asio::ip::address> Loopback()
{
    std::vector<boost::asio::ip::address> addresses;
    for (std::string const& address : LoopbackAddresses)
        addresses.push_back(boost::asio::ip::make_address(address));
    return addresses;
}

// On every response, pages and API alike. Inline scripts: the theme script and React Router's
// hydration data. Inline styles: Shiki's colors
std::vector<std::pair<std::string, std::string>> const SecurityHeaders =
{
    { "X-Content-Type-Options", "nosniff" },
    { "X-Frame-Options", "DENY" },
    { "Referrer-Policy", "strict-origin-when-cross-origin" },
    { "Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' https: data:; "
        "connect-src 'self'; frame-ancestors 'none'; object-src 'none'; base-uri 'self'; form-action 'self'" },
};

// Browsers remember to use https for a year: only sent over TLS
std::pair<std::string, std::string> const StrictTransportSecurity = { "Strict-Transport-Security", "max-age=31536000; includeSubDomains" };

// Where a service listens: Server.Http.* or Server.Https.*
struct Endpoint
{
    std::string BindIp;
    uint16 Port = 0;
    int32 NetworkThreads = 2;
};

Endpoint LoadEndpoint(Config const& config, std::string_view service, std::string const& defaultBindIp, uint16 defaultPort)
{
    std::string prefix(service);
    Endpoint endpoint;
    endpoint.BindIp = config.Get<std::string>(Blog::ServerKey(config, prefix + ".BindIP"), defaultBindIp);
    endpoint.Port = config.Get<uint16>(Blog::ServerKey(config, prefix + ".Port"), defaultPort);
    endpoint.NetworkThreads = config.Get<int32>(Blog::ServerKey(config, prefix + ".NetworkThreads"), endpoint.NetworkThreads);
    return endpoint;
}

// What the plain and the TLS service share
WorldHttpSettings LoadHttpSettings(Config const& config)
{
    WorldHttpSettings settings;
    for (std::string const& proxy : config.Get<std::vector<std::string>>(Blog::ServerKey(config, "Http.TrustedProxies"), LoopbackAddresses))
    {
        boost::system::error_code error;
        boost::asio::ip::address address = boost::asio::ip::make_address(proxy, error);
        if (error)
            RT_LOG_ERROR("blog", "Server.Http.TrustedProxies: {} is not an address, ignored", proxy);
        else
            settings.TrustedProxies.push_back(address);
    }

    settings.MaxRequestBodySize = config.Get<uint64>(Blog::ServerKey(config, "Http.MaxRequestBodySize"), 11 * 1024 * 1024);
    settings.RequestTimeout = Seconds(config.Get<uint32>(Blog::ServerKey(config, "Http.RequestTimeout"), 60));
    settings.KeepAliveTimeout = Seconds(config.Get<uint32>(Blog::ServerKey(config, "Http.KeepAliveTimeout"), 75));
    settings.MaxConnectionsPerAddress = config.Get<uint32>(Blog::ServerKey(config, "Http.MaxConnectionsPerIp"), 64);
    settings.ConnectionLimitExempt = Loopback();
    settings.WebSocketIdleTimeout = Seconds(config.Get<uint32>(Blog::ServerKey(config, "WebSocket.IdleTimeout"), 90));
    settings.WebSocketMaxWriteQueueSize = config.Get<uint32>(Blog::ServerKey(config, "WebSocket.MaxWriteQueueSize"), 1024 * 1024);
    return settings;
}

// The same routes on the plain and the TLS service
void SetUpService(WorldHttpService& service, Blog::BlogWorld const& world, Rooftree::Server::WebSocketRouter const& webSocketRouter,
    std::string const& acmeDirectory)
{
    std::vector<std::pair<std::string, std::string>> headers = SecurityHeaders;
    if (service.IsSsl())
        headers.push_back(StrictTransportSecurity);
    service.SetDefaultHeaders(std::move(headers));

    Blog::RegisterRoutes(service);
    Blog::RegisterWebSocketRoute(service, webSocketRouter, world.GetSettings().AllowedOrigins, world.GetSettings().CookieName);
    Blog::RegisterStaticRoutes(service, world.GetStaticSite(), world.GetSettings().UploadDirectory, acmeDirectory);
}

// Plain HTTP: the API behind the vite dev server, or port 80 sending readers to https
void SetUpHttp(WorldHttpService& http, Config const& config, Blog::BlogWorld const& world, WorldHttpSettings settings)
{
    if (config.Get<bool>(Blog::ServerKey(config, "Http.RedirectToHttps"), false))
    {
        settings.RedirectToOrigin = world.GetSettings().PublicOrigin;
        settings.RedirectExempt = Loopback();
    }
    http.SetSettings(std::move(settings));
}

// Server.Https: false (and logged) without a certificate that loads
bool SetUpHttps(WorldHttpService& https, Config const& config, WorldHttpSettings settings)
{
    settings.SslCertificateFile = config.Get<std::string>(Blog::ServerKey(config, "Https.CertificateFile"), "");
    settings.SslPrivateKeyFile = config.Get<std::string>(Blog::ServerKey(config, "Https.PrivateKeyFile"), "");
    std::string error;
    if (settings.SslCertificateFile.empty() || settings.SslPrivateKeyFile.empty())
        error = "CertificateFile and PrivateKeyFile are required";

    https.SetSettings(std::move(settings));
    if (!error.empty() || !https.LoadSslContext(error))
    {
        RT_LOG_FATAL("blog", "Server.Https: {}", error);
        return false;
    }
    return true;
}

// Signals and stopping

// Every signal of the set on the io thread, until the io_context stops
void WaitForSignals(SignalSet& signals, std::function<void()> onSignal)
{
    signals.async_wait([&signals, onSignal = std::move(onSignal)](boost::system::error_code const& error, int) mutable
    {
        if (error)
            return;

        onSignal();
        WaitForSignals(signals, std::move(onSignal));
    });
}

// SIGHUP: the config at the next update, and a renewed certificate right away
void Reload(Blog::BlogWorld& world, WorldHttpService* https)
{
    RT_LOG_INFO("blog", "SIGHUP: reloading the config");
    world.RequestReload();

    std::string tlsError;
    if (https && !https->LoadSslContext(tlsError))
        RT_LOG_ERROR("blog", "Server.Https: {}, the current certificate stays", tlsError);
    else if (https)
        RT_LOG_INFO("blog", "Certificate reloaded");
}

// SIGINT/SIGTERM (docker stop, systemctl stop): counted on the io thread, acted on in the main loop.
// The first one stops after Server.ShutdownDelay, the pages and players told meanwhile, or keeps the
// stop scheduled already; another one stops right away
class StopRequests
{
public:
    // Any thread
    void Add() { ++_received; }

    // Main thread, before the world's update
    void Update(Rooftree::Server::UpdateLoop& loop, Milliseconds shutdownDelay)
    {
        uint32 received = _received.load();
        if (received == _handled)
            return;

        bool first = !_handled;
        _handled = received;
        Optional<TimePoint> scheduled = loop.GetScheduledStop();
        if (first && shutdownDelay > 0ms && (!scheduled || *scheduled > TimePoint::clock::now() + shutdownDelay))
        {
            RT_LOG_INFO("blog", "Stopping in {} s (Server.ShutdownDelay), the pages are told; stop again to stop now",
                std::chrono::duration_cast<Seconds>(shutdownDelay).count());
            loop.StopAfter(shutdownDelay);
        }
        else if (first && scheduled)
            RT_LOG_INFO("blog", "Stopping as scheduled already; stop again to stop now");
        else
            loop.Stop();
    }

private:
    std::atomic<uint32> _received = 0;
    uint32 _handled = 0;
};

// Once a stop is scheduled (a SIGTERM with Server.ShutdownDelay, the console's server restart), every
// page is told, again at 600, 300, 120, 60, 30, 10 and 5 seconds left, and when it is cancelled
class RestartNotice
{
public:
    void Update(Optional<TimePoint> const& stopAt, Blog::BlogWorld& world)
    {
        if (!stopAt)
        {
            if (_announcedFor)
                world.AnnounceRestart({});
            _announcedFor.reset();
            return;
        }

        TimePoint now = TimePoint::clock::now();
        uint32 left = *stopAt > now ? uint32((std::chrono::duration_cast<Milliseconds>(*stopAt - now).count() + 999) / 1000) : 0;
        // After the last mark (5 s) nothing more: the pages see it go
        if (_announcedFor == stopAt && (!_next || left > _next))
            return;

        world.AnnounceRestart(left);
        _announcedFor = stopAt;
        _next = 0;
        for (uint32 mark : { 600u, 300u, 120u, 60u, 30u, 10u, 5u })
            if (mark < left)
            {
                _next = mark;
                break;
            }
    }

private:
    Optional<TimePoint> _announcedFor;
    uint32 _next = 0;
};

// The main loop's settings, at startup and after a reload
void ApplyLoopSettings(Rooftree::Server::UpdateLoop& loop, Blog::BlogSettings const& settings)
{
    loop.SetMinUpdateDiff(settings.MinUpdateDiff);
    loop.SetStatsInterval(settings.StatsInterval);
}

// Commands typed on the server's terminal, and on a unix socket for blog --console (ConsoleCommands.h)
void StartConsole(Config const& config, Blog::BlogWorld& world, Rooftree::Asio::IoContext& ioContext,
    std::optional<Rooftree::Server::ConsoleThread>& console, std::optional<Rooftree::Server::ConsoleSocketServer>& consoleSocket)
{
    if (!config.Get<bool>(Blog::ServerKey(config, "Console.Enable"), true))
        return;

    if (Rooftree::Server::ConsoleThread::CanStart())
    {
        console.emplace(world, Blog::GetConsoleCommands());
        console->Start();
    }

    std::string socketPath = config.Get<std::string>(Blog::ServerKey(config, "Console.Socket"), "console.sock");
    if (socketPath.empty())
        return;

    std::string socketError;
    consoleSocket.emplace(world, Blog::GetConsoleCommands());
    if (!consoleSocket->Start(ioContext, socketPath, socketError))
    {
        RT_LOG_ERROR("blog", "Server.Console.Socket: {}, no blog --console", socketError);
        consoleSocket.reset();
    }
    else
        RT_LOG_INFO("blog", "Console on {}: blog --console", std::filesystem::absolute(socketPath).string());
}

// The services, the io thread and the update loop, with the world loaded; returns the exit code
int Serve(Config const& config, Blog::BlogWorld& world)
{
    Rooftree::Server::WebSocketRouter webSocketRouter(world);
    Blog::RegisterWebSocketRoutes(webSocketRouter);

    std::string acmeDirectory = config.Get<std::string>(Blog::ServerKey(config, "Http.AcmeChallengeDirectory"), "");
    WorldHttpSettings httpSettings = LoadHttpSettings(config);
    Endpoint plainAt = LoadEndpoint(config, "Http", "127.0.0.1", 8100);
    Endpoint tlsAt = LoadEndpoint(config, "Https", plainAt.BindIp, 0);

    WorldHttpService http("blog", world);
    SetUpHttp(http, config, world, httpSettings);
    SetUpService(http, world, webSocketRouter, acmeDirectory);

    // The TLS one has the same routes
    std::optional<WorldHttpService> https;
    if (tlsAt.Port)
    {
        https.emplace("blog.https", world);
        if (!SetUpHttps(*https, config, httpSettings))
            return 1;
        SetUpService(*https, world, webSocketRouter, acmeDirectory);
    }

    Rooftree::Asio::IoContext ioContext;
    Rooftree::Server::UpdateLoop loop;
    ApplyLoopSettings(loop, world.GetSettings());
    world.SetOnReloaded([&loop, &world](Config const&) { ApplyLoopSettings(loop, world.GetSettings()); });

    StopRequests stopRequests;
    SignalSet stopSignals(ioContext, SIGINT, SIGTERM);
    WaitForSignals(stopSignals, [&stopRequests] { stopRequests.Add(); });
    SignalSet reloadSignals(ioContext, SIGHUP);
    WaitForSignals(reloadSignals, [&world, tls = https ? &*https : nullptr] { Reload(world, tls); });

    if (!http.StartNetwork(ioContext, plainAt.BindIp, plainAt.Port, plainAt.NetworkThreads))
        return 1;

    if (https && !https->StartNetwork(ioContext, tlsAt.BindIp, tlsAt.Port, tlsAt.NetworkThreads))
    {
        http.StopNetwork();
        return 1;
    }

    std::shared_ptr<Rooftree::Server::FreezeDetector> freezeDetector;
    if (uint32 maxCoreStuckTime = config.Get<uint32>(Blog::ServerKey(config, "MaxCoreStuckTime"), 60))
        freezeDetector = Rooftree::Server::FreezeDetector::Start(ioContext, loop, Seconds(maxCoreStuckTime));

    // The acceptor and the signal sets keep it busy, the guard keeps it running if they ever don't
    auto workGuard = boost::asio::make_work_guard(ioContext);
    std::thread ioThread([&ioContext] { ioContext.run(); });

    RT_LOG_INFO("blog", "Listening on http://{}:{}/ (the API, /ws), Ctrl+C to stop", plainAt.BindIp, plainAt.Port);
    if (https)
        RT_LOG_INFO("blog", "Listening on https://{}:{}/", tlsAt.BindIp, tlsAt.Port);

    Blog::SetConsoleUpdateLoop(&loop);
    Blog::SetConsoleApi(&http, plainAt.BindIp, plainAt.Port);
    std::optional<Rooftree::Server::ConsoleThread> console;
    std::optional<Rooftree::Server::ConsoleSocketServer> consoleSocket;
    StartConsole(config, world, ioContext, console, consoleSocket);

    RestartNotice restartNotice;
    loop.Run([&](uint32 diff)
    {
        stopRequests.Update(loop, world.GetSettings().ShutdownDelay);
        world.Update(diff);
        restartNotice.Update(loop.GetScheduledStop(), world);
    });

    RT_LOG_INFO("blog", "Shutting down");
    console.reset();
    freezeDetector = nullptr;
    if (https)
        https->StopNetwork();
    http.StopNetwork();
    workGuard.reset();
    ioContext.stop();
    ioThread.join();
    consoleSocket.reset();
    // They point into this function
    world.SetOnReloaded({});
    Blog::SetConsoleUpdateLoop(nullptr);
    Blog::SetConsoleApi(nullptr, {}, 0);

    // server restart's
    return loop.GetExitCode();
}

// The world around Serve: loaded before the ports open, so the first request has it all, and shut
// down after they closed
int RunServer(Config const& config, Blog::BlogSettings settings)
{
    Blog::BlogWorld world(config.Get<uint32>(Blog::ServerKey(config, "WorkerThreads"), 4));
    Blog::sWorld = &world;
    world.LoadSettings(std::move(settings));
    world.LoadFromDatabase();

    int exitCode = Serve(config, world);

    world.Shutdown();
    Blog::sWorld = nullptr;
    return exitCode;
}
}

int main(int argc, char** argv)
{
    Optional<Arguments> arguments = ParseArguments(argc, argv);
    if (!arguments)
        return 1;

    std::string configError;
    if (!sConfigMgr->LoadFile(arguments->ConfigFile, configError))
    {
        std::print(stderr, "Could not load {}: {}\n", arguments->ConfigFile, configError);
        return 1;
    }

    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();
    // A client: no logs, no database
    if (arguments->Console)
    {
        std::string socket = arguments->ConsoleSocket.empty() ? config->Get<std::string>(Blog::ServerKey(*config, "Console.Socket"), "console.sock") : arguments->ConsoleSocket;
        return Rooftree::Server::RunConsoleClient(socket, &Blog::GetConsoleCommands());
    }

    sLog->LoadFromConfig();
    Blog::BlogSettings settings;
    if (!settings.LoadFromConfig(*config) || !Blog::OpenDatabase(*config))
        return 1;

    if (arguments->CreateAdmin)
    {
        int exitCode = Blog::CreateAdmin(arguments->Admin, settings);
        Blog::CloseDatabase();
        return exitCode;
    }

    int exitCode = RunServer(*config, std::move(settings));
    Blog::CloseDatabase();
    RT_LOG_INFO("blog", "Stopped");
    return exitCode;
}
