/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The blog's API server:
 *
 *   load the config and the logs -> open the database, check the migrations -> load the settings and
 *   the tags -> start HTTP on its network threads -> FreezeDetector -> the update loop on this thread
 *   until SIGINT/SIGTERM -> stop the network, the sessions, the database
 *
 *   blog [--config blog.yaml]                     serve
 *   blog [--config blog.yaml] --create-admin --username NAME --email ADDRESS [--display-name NAME] [--totp]
 *                                                 create the author's account (password from
 *                                                 BLOG_ADMIN_PASSWORD or asked for); --totp: with
 *                                                 two-step login, its secret printed
 *   blog [--config blog.yaml] --console [--socket PATH]
 *                                                 the running server's console (Server.Console.Socket,
 *                                                 "help" lists the commands, ConsoleCommands.h)
 *
 * Serving on a terminal, it reads the same commands from it.
 *
 * kill -HUP reloads blog.yaml at the next update.
 */

#include "Argon2Hash.h"
#include "BlogDatabase.h"
#include "BlogRoutes.h"
#include "BlogSession.h"
#include "BlogWebSocket.h"
#include "BlogWorld.h"
#include "Config.h"
#include "Console.h"
#include "ConsoleApi.h"
#include "ConsoleCommands.h"
#include "ConsoleSocket.h"
#include "CryptoRandom.h"
#include "DatabaseConfig.h"
#include "Field.h"
#include "IoContext.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"
#include "TOTP.h"
#include "TotpSetup.h"
#include "UpdateLoop.h"
#include "Validation.h"
#include "WorldHttpService.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/signal_set.hpp>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <termios.h>
#include <thread>
#include <unistd.h>

namespace
{
using SignalSet = boost::asio::basic_signal_set<Rooftree::Asio::IoContextExecutor>;

struct Arguments
{
    std::string ConfigFile = "blog.yaml";
    bool CreateAdmin = false;
    bool Totp = false;
    bool Console = false;
    std::string ConsoleSocket;
    std::string Username;
    std::string Email;
    std::string DisplayName;
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
            arguments.Totp = true;
        else if (arg == "--console")
            arguments.Console = true;
        else if (arg == "--socket" && (v = value()))
            arguments.ConsoleSocket = *v;
        else if (arg == "--config" && (v = value()))
            arguments.ConfigFile = *v;
        else if (arg == "--username" && (v = value()))
            arguments.Username = *v;
        else if (arg == "--email" && (v = value()))
            arguments.Email = *v;
        else if (arg == "--display-name" && (v = value()))
            arguments.DisplayName = *v;
        else
        {
            fmt::print(stderr, "Unknown or incomplete argument {}\n", arg);
            return {};
        }
    }

    return arguments;
}

// dbmate keeps the applied versions in schema_migrations; each file of the migrations directory has to
// be there, statements are prepared against the tables they create
bool CheckMigrations(std::string const& directory)
{
    std::set<std::string> expected;
    std::error_code error;
    for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator(directory, error))
    {
        std::string name = entry.path().filename().string();
        if (entry.path().extension() == ".sql" && name.find('_') != std::string::npos)
            expected.insert(name.substr(0, name.find('_')));
    }

    if (error || expected.empty())
    {
        RT_LOG_ERROR("blog", "No migrations found in {} (Blog.MigrationsDirectory)", directory);
        return false;
    }

    // A plain query: before the first migration schema_migrations doesn't exist, which Query logs
    QueryResult result = BlogDatabase.Query("SELECT version FROM schema_migrations");
    if (result)
    {
        do
            expected.erase(result->Fetch()[0].GetString());
        while (result->NextRow());
    }

    if (!expected.empty())
    {
        RT_LOG_ERROR("blog", "The database lacks {} migration(s), starting with {}. Apply them first: pnpm db:up in apps/blog",
            expected.size(), *expected.begin());
        return false;
    }

    return true;
}

bool StartDB(Config const& config)
{
    std::optional<DatabaseConfig> database = config.Get<DatabaseConfig>("Database.Blog");
    if (!database)
    {
        RT_LOG_ERROR("blog", "Database.Blog is not set or invalid, it needs at least Host, User and Name");
        return false;
    }


    BlogDatabase.SetConnectionInfo(database->Connection, database->WorkerThreads, database->SynchThreads);
    if (BlogDatabase.Open())
    {
        RT_LOG_ERROR("blog", "Could not connect to the blog database");
        return false;
    }

    if (!CheckMigrations(config.Get<std::string>("Blog.MigrationsDirectory", "blog-migrations")) || !BlogDatabase.PrepareStatements())
    {
        BlogDatabase.Close();
        return false;
    }

    return true;
}

void StopDB()
{
    BlogDatabase.Close();
}

std::string ReadPassword(char const* prompt)
{
    std::cerr << prompt << std::flush;
    termios old{};
    bool terminal = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (terminal)
    {
        termios silent = old;
        silent.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &silent);
    }

    std::string password;
    std::getline(std::cin, password);
    if (terminal)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        std::cerr << '\n';
    }

    return password;
}

int CreateAdmin(Arguments const& arguments, Blog::BlogSettings const& settings)
{
    std::string displayName = arguments.DisplayName.empty() ? arguments.Username : arguments.DisplayName;
    if (!Blog::IsValidUsername(arguments.Username) || !Blog::IsValidEmail(arguments.Email)
        || displayName.empty() || Blog::CountCharacters(displayName) > 40)
    {
        fmt::print(stderr, "--username needs 3 to 20 letters, digits or _, --email an address, --display-name at most 40 characters\n");
        return 1;
    }

    PreparedQueryResult admins = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_EXISTS));
    if (admins && admins->Fetch()[0].GetInt64() > 0)
    {
        fmt::print(stderr, "There is an admin already, the blog has a single author. Its password and two-step login are set on the\n"
            "server's console: account set password, account set 2fa\n");
        return 1;
    }

    std::string password;
    if (char const* fromEnvironment = std::getenv("BLOG_ADMIN_PASSWORD"))
        password = fromEnvironment;
    else
    {
        password = ReadPassword("Password: ");
        if (ReadPassword("Again: ") != password)
        {
            fmt::print(stderr, "The passwords differ\n");
            return 1;
        }
    }

    if (Blog::CountCharacters(password) < 12 || password.size() > 128)
    {
        fmt::print(stderr, "The admin password needs 12 to 128 characters\n");
        return 1;
    }
    Optional<std::string> hash = Rooftree::Crypto::Argon2::Hash(password, settings.Argon2Iterations, settings.Argon2MemoryCost);
    if (!hash)
    {
        fmt::print(stderr, "Hashing the password failed\n");
        return 1;
    }

    std::vector<uint8> secret;
    if (arguments.Totp)
    {
        secret.resize(Rooftree::Crypto::TOTP::SecretSize);
        Rooftree::Crypto::GetRandomBytes(secret);
    }

    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_USER);
    stmt->setString(0, arguments.Username);
    stmt->setString(1, displayName);
    stmt->setString(2, arguments.Email);
    stmt->setString(3, *hash);
    stmt->setString(4, Blog::RoleAdmin);
    stmt->setString(5, std::string_view("zh"));
    stmt->setDate(6, SystemTimePoint::clock::now());
    if (arguments.Totp)
        stmt->setBinary(7, std::span<uint8 const>(secret));
    else
        stmt->setNull(7);
    BlogDatabase.DirectExecute(stmt);

    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_ID_AND_HASH_BY_NAME);
    stmt->setString(0, arguments.Username);
    PreparedQueryResult created = BlogDatabase.Query(stmt);
    if (!created || created->Fetch()[1].GetString() != *hash)
    {
        fmt::print(stderr, "The user name or the address is taken\n");
        return 1;
    }

    fmt::print("Created admin {} ({}).\n", arguments.Username, created->Fetch()[0].GetUInt32());
    if (arguments.Totp)
        fmt::print("\n{}\n", Blog::FormatTotpSetup(settings.TotpIssuer, arguments.Username, secret, isatty(STDOUT_FILENO)));
    return 0;
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

Rooftree::Server::WorldHttpSettings LoadHttpSettings(Config const& config)
{
    Rooftree::Server::WorldHttpSettings settings;
    for (std::string const& proxy : config.Get<std::vector<std::string>>(Blog::ServerKey(config, "Http.TrustedProxies"), { "127.0.0.1", "::1" }))
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
    // The rebuild's prerendering reads every page from 127.0.0.1
    settings.ConnectionLimitExempt = { boost::asio::ip::make_address("127.0.0.1"), boost::asio::ip::make_address("::1") };
    settings.WebSocketIdleTimeout = Seconds(config.Get<uint32>(Blog::ServerKey(config, "WebSocket.IdleTimeout"), 90));
    settings.WebSocketMaxWriteQueueSize = config.Get<uint32>(Blog::ServerKey(config, "WebSocket.MaxWriteQueueSize"), 1024 * 1024);
    return settings;
}

// Browsers remember to use https for a year: only sent over TLS
std::pair<std::string, std::string> const StrictTransportSecurity = { "Strict-Transport-Security", "max-age=31536000; includeSubDomains" };

// The same routes on the plain and the TLS service
void SetUpService(Rooftree::Server::WorldHttpService& service, Blog::BlogWorld const& world, Rooftree::Server::WebSocketRouter const& webSocketRouter,
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

// Once a stop is scheduled (a SIGTERM with Server.ShutdownDelay, server
// restart), every page is told, again at 60, 30, 10 and 5 seconds left, and when it is cancelled
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

// SIGHUP: the config at the next update, and a renewed certificate right away
void WaitForReloadSignal(SignalSet& signals, Rooftree::Server::WorldHttpService* https)
{
    signals.async_wait([&signals, https](boost::system::error_code const& error, int)
    {
        if (error)
            return;

        RT_LOG_INFO("blog", "SIGHUP: reloading the config");
        Blog::sWorld->RequestReload();

        std::string tlsError;
        if (https && !https->LoadSslContext(tlsError))
            RT_LOG_ERROR("blog", "Server.Https: {}, the current certificate stays", tlsError);
        else if (https)
            RT_LOG_INFO("blog", "Certificate reloaded");

        WaitForReloadSignal(signals, https);
    });
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
        fmt::print(stderr, "Could not load {}: {}\n", arguments->ConfigFile, configError);
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
    if (!settings.LoadFromConfig(*config) || !StartDB(*config))
        return 1;

    if (arguments->CreateAdmin)
    {
        int result = CreateAdmin(*arguments, settings);
        StopDB();
        return result;
    }

    Blog::BlogWorld world(config->Get<uint32>(Blog::ServerKey(*config, "WorkerThreads"), 4));
    Blog::sWorld = &world;
    world.LoadSettings(std::move(settings));
    world.SetTags(Blog::LoadTagsDirect());
    world.SetIpBans(Blog::LoadIpBansDirect());
    world.SetSiteProfile(Blog::LoadSiteProfileDirect());
    Blog::CountMissingWordsDirect();
    // Loaded before the ports open: the first search has an index
    TimePoint indexStart = TimePoint::clock::now();
    std::unique_ptr<Blog::SearchIndex> searchIndex = Blog::SearchIndex::Load();
    RT_LOG_INFO("blog", "Search index of {} documents built in {} ms", searchIndex->GetDocumentCount(),
        std::chrono::duration_cast<Milliseconds>(TimePoint::clock::now() - indexStart).count());
    world.SetSearchIndex(std::move(searchIndex));
    world.SetCommentCounts(Blog::LoadCommentCountsDirect());
    world.LoadStaticSite(true);

    Rooftree::Server::WebSocketRouter webSocketRouter(world);
    Blog::RegisterWebSocketRoutes(webSocketRouter);

    // Plain HTTP: the API behind the vite dev server, or port 80 sending readers to https. The TLS one
    // has the same routes
    std::string acmeDirectory = config->Get<std::string>(Blog::ServerKey(*config, "Http.AcmeChallengeDirectory"), "");
    Rooftree::Server::WorldHttpSettings httpSettings = LoadHttpSettings(*config);
    Rooftree::Server::WorldHttpService http("blog", world);
    {
        Rooftree::Server::WorldHttpSettings plain = httpSettings;
        if (config->Get<bool>(Blog::ServerKey(*config, "Http.RedirectToHttps"), false))
        {
            plain.RedirectToOrigin = world.GetSettings().PublicOrigin;
            // The host's own scripts (the rebuild's prerendering) call the API on 127.0.0.1
            plain.RedirectExempt = { boost::asio::ip::make_address("127.0.0.1"), boost::asio::ip::make_address("::1") };
        }
        http.SetSettings(std::move(plain));
    }
    SetUpService(http, world, webSocketRouter, acmeDirectory);

    uint16 httpsPort = config->Get<uint16>(Blog::ServerKey(*config, "Https.Port"), 0);
    std::optional<Rooftree::Server::WorldHttpService> https;
    if (httpsPort)
    {
        Rooftree::Server::WorldHttpSettings tls = httpSettings;
        tls.SslCertificateFile = config->Get<std::string>(Blog::ServerKey(*config, "Https.CertificateFile"), "");
        tls.SslPrivateKeyFile = config->Get<std::string>(Blog::ServerKey(*config, "Https.PrivateKeyFile"), "");
        std::string error;
        if (tls.SslCertificateFile.empty() || tls.SslPrivateKeyFile.empty())
            error = "CertificateFile and PrivateKeyFile are required";

        https.emplace("blog.https", world);
        https->SetSettings(std::move(tls));
        if (!error.empty() || !https->LoadSslContext(error))
        {
            RT_LOG_FATAL("blog", "Server.Https: {}", error);
            world.Shutdown();
            StopDB();
            return 1;
        }
        SetUpService(*https, world, webSocketRouter, acmeDirectory);
    }

    Rooftree::Asio::IoContext ioContext;
    Rooftree::Server::UpdateLoop loop(Milliseconds(config->Get<uint32>(Blog::ServerKey(*config, "MinUpdateDiff"), 2)));
    loop.SetStatsInterval(Seconds(config->Get<uint32>(Blog::ServerKey(*config, "StatsInterval"), 60)));

    // SIGTERM (docker stop, systemctl stop): the main loop stops after Server.ShutdownDelay, the pages and
    // players told meanwhile; another one stops it right away. Counted here, acted on in the main loop
    std::atomic<uint32> stopSignalCount = 0;
    SignalSet stopSignals(ioContext, SIGINT, SIGTERM);
    std::function<void()> waitForStop = [&]
    {
        stopSignals.async_wait([&](boost::system::error_code const& error, int)
        {
            if (error)
                return;
            ++stopSignalCount;
            waitForStop();
        });
    };
    waitForStop();
    Milliseconds shutdownDelay = Seconds(config->Get<uint32>(Blog::ServerKey(*config, "ShutdownDelay"), 0));

    // What a reload changes outside the world
    world.SetOnReloaded([&loop, &shutdownDelay](Config const& reloaded)
    {
        loop.SetMinUpdateDiff(Milliseconds(reloaded.Get<uint32>(Blog::ServerKey(reloaded, "MinUpdateDiff"), 2)));
        loop.SetStatsInterval(Seconds(reloaded.Get<uint32>(Blog::ServerKey(reloaded, "StatsInterval"), 60)));
        shutdownDelay = Seconds(reloaded.Get<uint32>(Blog::ServerKey(reloaded, "ShutdownDelay"), 0));
    });

    SignalSet reloadSignals(ioContext, SIGHUP);
    WaitForReloadSignal(reloadSignals, https ? &*https : nullptr);

    std::string bindIp = config->Get<std::string>(Blog::ServerKey(*config, "Http.BindIP"), "127.0.0.1");
    uint16 port = config->Get<uint16>(Blog::ServerKey(*config, "Http.Port"), 8100);
    if (!http.StartNetwork(ioContext, bindIp, port, config->Get<int32>(Blog::ServerKey(*config, "Http.NetworkThreads"), 2)))
    {
        world.Shutdown();
        StopDB();
        return 1;
    }

    std::string httpsBindIp = config->Get<std::string>(Blog::ServerKey(*config, "Https.BindIP"), bindIp);
    if (https && !https->StartNetwork(ioContext, httpsBindIp, httpsPort, config->Get<int32>(Blog::ServerKey(*config, "Https.NetworkThreads"), 2)))
    {
        http.StopNetwork();
        world.Shutdown();
        StopDB();
        return 1;
    }

    std::shared_ptr<Rooftree::Server::FreezeDetector> freezeDetector;
    if (uint32 maxCoreStuckTime = config->Get<uint32>(Blog::ServerKey(*config, "MaxCoreStuckTime"), 60))
        freezeDetector = Rooftree::Server::FreezeDetector::Start(ioContext, loop, Seconds(maxCoreStuckTime));

    // The acceptor and the signal sets keep it busy, the guard keeps it running if they ever don't
    auto workGuard = boost::asio::make_work_guard(ioContext);
    std::thread ioThread([&ioContext] { ioContext.run(); });

    RT_LOG_INFO("blog", "Listening on http://{}:{}/ (the API, /ws), Ctrl+C to stop", bindIp, port);
    if (https)
        RT_LOG_INFO("blog", "Listening on https://{}:{}/", httpsBindIp, httpsPort);

    // Commands typed on the server's terminal, and on a unix socket for blog --console
    // (ConsoleCommands.h)
    Blog::SetConsoleUpdateLoop(&loop);
    Blog::SetConsoleApi(&http, bindIp, port);
    std::optional<Rooftree::Server::ConsoleThread> console;
    std::optional<Rooftree::Server::ConsoleSocketServer> consoleSocket;
    if (config->Get<bool>(Blog::ServerKey(*config, "Console.Enable"), true))
    {
        if (Rooftree::Server::ConsoleThread::CanStart())
        {
            console.emplace(world, Blog::GetConsoleCommands());
            console->Start();
        }

        std::string socketPath = config->Get<std::string>(Blog::ServerKey(*config, "Console.Socket"), "console.sock");
        std::string socketError;
        consoleSocket.emplace(world, Blog::GetConsoleCommands());
        if (socketPath.empty())
            consoleSocket.reset();
        else if (!consoleSocket->Start(ioContext, socketPath, socketError))
        {
            RT_LOG_ERROR("blog", "Server.Console.Socket: {}, no blog --console", socketError);
            consoleSocket.reset();
        }
        else
            RT_LOG_INFO("blog", "Console on {}: blog --console", std::filesystem::absolute(socketPath).string());
    }

    RestartNotice restartNotice;
    uint32 stopSignalsHandled = 0;
    loop.Run([&](uint32 diff)
    {
        if (uint32 signals = stopSignalCount.load(); signals != stopSignalsHandled)
        {
            Optional<TimePoint> scheduled = loop.GetScheduledStop();
            if (!stopSignalsHandled && shutdownDelay > 0ms && (!scheduled || *scheduled > TimePoint::clock::now() + shutdownDelay))
            {
                RT_LOG_INFO("blog", "Stopping in {} s (Server.ShutdownDelay), the pages are told; stop again to stop now",
                    std::chrono::duration_cast<Seconds>(shutdownDelay).count());
                loop.StopAfter(shutdownDelay);
            }
            else if (!stopSignalsHandled && scheduled)
                RT_LOG_INFO("blog", "Stopping as scheduled already; stop again to stop now");
            else
                loop.Stop();
            stopSignalsHandled = signals;
        }

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

    world.Shutdown();
    Blog::sWorld = nullptr;
    StopDB();
    RT_LOG_INFO("blog", "Stopped");
    // server restart's
    return loop.GetExitCode();
}
