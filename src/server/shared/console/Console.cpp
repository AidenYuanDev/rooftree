/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Console.h"
#include "Log.h"
#include "SessionMgr.h"
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <poll.h>
#include <termios.h>
#include <typeinfo>
#include <unistd.h>

namespace Rooftree::Server
{
namespace
{
std::string const TerminalSessionKey = "<console>";
}

ConsoleMessage::ConsoleMessage(MessageType const& type, std::string sessionKey, std::shared_ptr<ConsoleOutput> output, CommandTable const& commands,
    std::string line, Kind kind)
    : Message(type, std::move(sessionKey), boost::asio::ip::address_v4::loopback()), _output(std::move(output)), _commands(commands),
    _line(std::move(line)), _kind(kind)
{
}

void ConsoleMessage::Process(Session& session)
{
    ConsoleSession* console = dynamic_cast<ConsoleSession*>(&session);
    if (!console)
    {
        RT_LOG_ERROR("server.console", "The console's messages went to a {}, CreateSession must make a ConsoleSession for them",
            typeid(session).name());
        return;
    }

    if (_kind == Kind::Closed)
    {
        // A running command still finishes, into an output nobody reads
        console->_answer = nullptr;
        console->Close();
        return;
    }

    if (console->_answer)
        return std::exchange(console->_answer, nullptr)(std::move(_line));

    // Until the handler is gone, asynchronous work included
    console->_running = true;
    std::shared_ptr<ConsoleOutput> output = _output;
    auto handler = std::make_shared<CommandHandler>(session,
        [output](std::string_view text) { output->Print(text); },
        [console, output]
        {
            console->_running = false;
            output->Ready();
        },
        [console, output](std::string_view prompt, bool secret, std::function<void(std::string)> answer)
        {
            console->_answer = std::move(answer);
            output->Ask(prompt, secret);
        });

    std::string name = _commands.Execute(*handler, _line);
    if (!name.empty())
        RT_LOG_INFO("server.console", "Console {}: {}", GetSessionKey(), name);
}

void ConsoleMessage::Reject(RejectReason /*reason*/)
{
    if (_kind == Kind::Closed)
        return;

    _output->Print("The server is busy, the command did not run");
    _output->Ready();
}

bool IsConsoleMessage(Message const& message)
{
    return dynamic_cast<ConsoleMessage const*>(&message) != nullptr;
}

// stdout, and a secret answer typed with the terminal's echo off
class ConsoleThread::TerminalOutput final : public ConsoleOutput
{
public:
    explicit TerminalOutput(std::string prompt) : _prompt(std::move(prompt)) { }

    void Print(std::string_view text) override
    {
        std::scoped_lock lock(_mutex);
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    void Ask(std::string_view prompt, bool secret) override
    {
        std::scoped_lock lock(_mutex);
        if (secret && tcgetattr(STDIN_FILENO, &_echoing) == 0)
        {
            termios silent = _echoing;
            silent.c_lflag &= ~tcflag_t(ECHO);
            _echoOff = tcsetattr(STDIN_FILENO, TCSANOW, &silent) == 0;
        }
        _asking = true;
        std::fwrite(prompt.data(), 1, prompt.size(), stdout);
        std::fflush(stdout);
    }

    void Ready() override
    {
        std::scoped_lock lock(_mutex);
        std::fwrite(_prompt.data(), 1, _prompt.size(), stdout);
        std::fflush(stdout);
    }

    // The console thread, after a line: whether it answers a question. The echo is back on
    bool TakeAnswer()
    {
        std::scoped_lock lock(_mutex);
        RestoreEcho();
        return std::exchange(_asking, false);
    }

    void RestoreEcho()
    {
        if (!std::exchange(_echoOff, false))
            return;

        tcsetattr(STDIN_FILENO, TCSANOW, &_echoing);
        // The Enter of the answer wasn't echoed either
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

private:
    std::mutex _mutex;
    std::string _prompt;
    bool _asking = false;
    bool _echoOff = false;
    termios _echoing{};
};

ConsoleThread::ConsoleThread(SessionMgr& sessionMgr, CommandTable const& commands, std::string prompt)
    : _sessionMgr(sessionMgr), _type(sessionMgr.GetMessageTypes().Register("console")), _commands(commands),
    _output(std::make_shared<TerminalOutput>(std::move(prompt)))
{
}

ConsoleThread::~ConsoleThread()
{
    Stop();
}

bool ConsoleThread::CanStart()
{
    return isatty(STDIN_FILENO);
}

void ConsoleThread::Start()
{
    _stopped = false;
    _thread = std::thread([this] { Run(); });
}

void ConsoleThread::Stop()
{
    _stopped = true;
    if (_thread.joinable())
        _thread.join();
    _output->TakeAnswer();
}

void ConsoleThread::Run()
{
    _output->Ready();

    // poll() with a timeout instead of a blocking read: Stop() is noticed
    std::string buffer;
    while (!_stopped)
    {
        pollfd input{ .fd = STDIN_FILENO, .events = POLLIN, .revents = 0 };
        int ready = poll(&input, 1, 100);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            break;
        if (ready == 0)
            continue;

        char chunk[1024];
        ssize_t read = ::read(STDIN_FILENO, chunk, sizeof(chunk));
        if (read < 0 && errno == EINTR)
            continue;
        if (read <= 0)
        {
            RT_LOG_INFO("server.console", "End of the console's input, the console stops");
            break;
        }

        buffer.append(chunk, std::size_t(read));
        for (std::size_t end = buffer.find('\n'); end != std::string::npos; end = buffer.find('\n'))
        {
            std::string line = buffer.substr(0, end);
            buffer.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // An answer may be empty, a command may not
            if (!_output->TakeAnswer() && line.find_first_not_of(" \t") == std::string::npos)
            {
                _output->Ready();
                continue;
            }

            _sessionMgr.Enqueue(std::make_unique<ConsoleMessage>(_type, TerminalSessionKey, _output, _commands, std::move(line)));
        }
    }
}
}
