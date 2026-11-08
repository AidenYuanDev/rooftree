/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * arena's HTTP side, modeled after bnetserver's LoginRESTService:
 *
 *   GET  /                  the browser client (Arena.WebRoot/index.html)
 *   POST /api/register      {"username", "password"} -> {"ok": true} or {"error"}
 *   POST /api/login         {"username", "password"} -> {"ticket", "name", "wsPort", "wsUrl"}
 *   GET  /api/leaderboard   -> {"players": [{"name", "coins"}]}
 *
 * Argon2 and the database queries take milliseconds, longer than a network thread may be blocked. So
 * those handlers return RequestHandlerResult::Async: the work runs on a ThreadPool, and the finished
 * response is posted back to the connection's network thread, the only thread that may queue on it.
 */

#ifndef ARENA_AUTH_SERVICE_H
#define ARENA_AUTH_SERVICE_H

#include "HttpService.h"
#include "HttpSocket.h"
#include "ThreadPool.h"
#include <boost/json/object.hpp>

class ArenaHttpSession;

struct AuthSettings
{
    std::string WebRoot = "arena-www";
    uint16 WebSocketPort = 8091;
    std::string WebSocketUrl;       // empty: the client builds ws://<page host>:<WebSocketPort>/arena
    uint16 KcpPort = 0;             // 0: KCP is off
    uint32 Argon2Iterations = 2;
    uint32 Argon2MemoryCost = 19 * 1024;    // KiB, OWASP's minimum for Argon2id
    Seconds TicketLifetime = 60s;
};

class AuthService final : public Rooftree::Net::Http::HttpService<ArenaHttpSession>
{
public:
    static AuthService& Instance();

    bool Start(Rooftree::Asio::IoContext& ioContext, std::string const& bindIp, uint16 port, int32 threadCount,
        AuthSettings settings, std::size_t workerThreads);
    void Stop();

private:
    AuthService();

    using RequestContext = Rooftree::Net::Http::RequestContext;
    using RequestHandlerResult = Rooftree::Net::Http::RequestHandlerResult;

    // Runs work(context) on the worker threads and sends its response afterwards
    RequestHandlerResult RunAsync(std::shared_ptr<ArenaHttpSession> session, RequestContext& context,
        std::function<void(RequestContext&)> work);

    RequestHandlerResult HandleGetIndex(std::shared_ptr<ArenaHttpSession> session, RequestContext& context);
    void HandleRegister(RequestContext& context);
    void HandleLogin(RequestContext& context);
    void HandleGetLeaderboard(RequestContext& context);

    AuthSettings _settings;
    std::unique_ptr<Rooftree::ThreadPool> _workers;
};

#define sAuthService AuthService::Instance()

class ArenaHttpSession final : public Rooftree::Net::Http::Socket
{
public:
    using Socket::Socket;

    Rooftree::Net::Http::RequestHandlerResult RequestHandler(Rooftree::Net::Http::RequestContext& context) override;
};

#endif // ARENA_AUTH_SERVICE_H
