/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The server's console on the session core:
 *
 *  - every line typed is handed to the SessionMgr as a ConsoleMessage, like a network thread hands a
 *    request. They are the messages of a session of their own,
 *    the ConsoleSession, so a command runs on the main thread like a request and may use OnWorker()
 *    (in a coroutine run by CommandHandler::Spawn) and AddQueryCallback() the same way
 *  - one command at a time per console: the next line waits until the one before is done, its
 *    asynchronous work included (CommandHandler), then the prompt is printed again
 *  - a command may ask for a line (CommandHandler::Ask): the next line answers instead of running
 *  - every command is logged to "server.console" by name, without its arguments: they may be passwords
 *
 * The lines come from the terminal the server runs on (ConsoleThread, here) or from clients of a unix
 * socket (ConsoleSocket.h); a ConsoleOutput is where each one's output goes. The app's
 * SessionMgr::CreateSession makes a ConsoleSession for a ConsoleMessage (IsConsoleMessage).
 *
 * ConsoleThread:
 *  - no readline: the terminal's own line editing (ConsoleSocket.h's client has readline)
 *  - started only when stdin is a terminal (CanStart). Under systemd or docker without a tty stdin is
 *    /dev/null. The end of the input (Ctrl+D) only ends the console, never the server
 */

#ifndef ROOFTREE_CONSOLE_H
#define ROOFTREE_CONSOLE_H

#include "ConsoleCommand.h"
#include "ServerSession.h"
#include <atomic>
#include <thread>

namespace Rooftree::Server
{
class SessionMgr;

// Where one console's output goes. Any thread: Reject() may call them from a network thread
class ConsoleOutput
{
public:
    virtual ~ConsoleOutput() = default;

    virtual void Print(std::string_view text) = 0;
    // The next line answers; secret: not echoed
    virtual void Ask(std::string_view prompt, bool secret) = 0;
    // Ready for a command: the one before is done, or there was none yet
    virtual void Ready() = 0;
};

class ConsoleSession final : public Session
{
public:
    ConsoleSession(SessionMgr& sessionMgr, std::string key, boost::asio::ip::address remoteAddress)
        : Session(sessionMgr, std::move(key), std::move(remoteAddress)) { }
    // A question nobody answered holds the handler, which ends its command here
    ~ConsoleSession() override { _answer = nullptr; }

protected:
    bool CanProcessMessages() const override { return !_running || _answer; }
    bool CanBeRemoved() const override { return !_running; }

private:
    friend class ConsoleMessage;

    bool _running = false;
    std::function<void(std::string)> _answer;   // a command asked for the next line
};

class ConsoleMessage final : public Message
{
public:
    enum class Kind
    {
        Line,
        Closed      // the client went away: its session is closed, a question dropped
    };

    // The table lives until the SessionMgr stopped. One session key per console
    ConsoleMessage(MessageType const& type, std::string sessionKey, std::shared_ptr<ConsoleOutput> output, CommandTable const& commands,
        std::string line, Kind kind = Kind::Line);

    void Process(Session& session) override;
    void Reject(RejectReason reason) override;

private:
    std::shared_ptr<ConsoleOutput> _output;
    CommandTable const& _commands;
    std::string _line;
    Kind _kind;
};

bool IsConsoleMessage(Message const& message);

class ConsoleThread
{
public:
    // Registers the "console" message type
    ConsoleThread(SessionMgr& sessionMgr, CommandTable const& commands, std::string prompt = "RT> ");
    ConsoleThread(ConsoleThread const&) = delete;
    ConsoleThread& operator=(ConsoleThread const&) = delete;
    ~ConsoleThread();

    // stdin is a terminal
    static bool CanStart();

    void Start();
    // Waits for the thread, it notices within 100 ms
    void Stop();

private:
    class TerminalOutput;

    void Run();

    SessionMgr& _sessionMgr;
    MessageType const& _type;
    CommandTable const& _commands;
    std::shared_ptr<TerminalOutput> _output;
    std::atomic<bool> _stopped = false;
    std::thread _thread;
};
}

#endif // ROOFTREE_CONSOLE_H
