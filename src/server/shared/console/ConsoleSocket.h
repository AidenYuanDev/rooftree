/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The console on a unix socket, for the host's own admin. Whoever may open the socket file is let in,
 * so it is created for its owner only (0600) and a client must run as the server's user or root
 * (checked with SO_PEERCRED as well). No port, no password.
 *
 *   server  ConsoleSocketServer, on the network's io context: every client is a console of its own,
 *           commands run as in Console.h
 *   client  RunConsoleClient, e.g. "blog --console": readline with history and Tab completion from
 *           the command table, answers to secret questions typed without echo. Ctrl+C or Ctrl+D end the
 *           client, never the server. Without a terminal it reads the commands from stdin, one per line
 *
 * The server's output (and what is typed) stays between the two: unlike a console on the server's own
 * terminal it doesn't end up in the server's log (docker logs, journald).
 *
 * Protocol, one JSON object per line. Client: {"line": "..."}, a command or the answer to a question.
 * Server: {"out": "..."} prints, {"ask": "Password: ", "secret": true} asks, {"ready": "RT> "} waits
 * for the next command.
 */

#ifndef ROOFTREE_CONSOLE_SOCKET_H
#define ROOFTREE_CONSOLE_SOCKET_H

#include "Console.h"
#include "Task.h"
#include <boost/asio/local/stream_protocol.hpp>
#include <mutex>

namespace Rooftree::Asio
{
class IoContext;
}

namespace Rooftree::Server
{
class ConsoleSocketServer
{
public:
    // Registers the "console" message type
    ConsoleSocketServer(SessionMgr& sessionMgr, CommandTable const& commands, std::string prompt = "RT> ");
    ConsoleSocketServer(ConsoleSocketServer const&) = delete;
    ConsoleSocketServer& operator=(ConsoleSocketServer const&) = delete;
    ~ConsoleSocketServer();

    // Listens on path. A socket file left over from before is replaced, one another server listens on
    // is an error
    bool Start(Asio::IoContext& ioContext, std::string const& path, std::string& error);
    // Once the io context stopped running: closes the clients and removes the file
    void Stop();

    static constexpr std::size_t MaxClients = 8;
    static constexpr std::size_t MaxLineSize = 64 * 1024;

private:
    class Connection;

    Rooftree::Detached AcceptLoop();

    SessionMgr& _sessionMgr;
    MessageType const& _type;
    CommandTable const& _commands;
    std::string _prompt;
    std::string _path;
    std::unique_ptr<boost::asio::local::stream_protocol::acceptor> _acceptor;
    std::mutex _connectionsMutex;
    std::vector<std::weak_ptr<Connection>> _connections;
    uint32 _nextId = 0;
};

// Until the server closes the connection or the input ends. completion: the commands to complete with
// Tab (the client's own copy of the server's table), may be null. The exit code for main
int RunConsoleClient(std::string const& path, CommandTable const* completion);
}

#endif // ROOFTREE_CONSOLE_SOCKET_H
