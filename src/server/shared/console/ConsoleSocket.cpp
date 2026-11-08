/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "ConsoleSocket.h"
#include "IoContext.h"
#include "Log.h"
#include "SessionMgr.h"
#include "Task.h"
#include <boost/asio/read_until.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <cerrno>
#include <cstring>
#include <deque>
#include <iostream>
#include <readline/history.h>
#include <readline/readline.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

using LocalSocket = boost::asio::local::stream_protocol::socket;

namespace Rooftree::Server
{
class ConsoleSocketServer::Connection final : public std::enable_shared_from_this<Connection>
{
public:
    Connection(ConsoleSocketServer& server, LocalSocket socket, std::string key)
        : _server(server), _socket(std::move(socket)), _key(std::move(key)), _buffer(MaxLineSize) { }

    void Start();
    void Send(boost::json::object message);
    // Any thread
    void Close();

private:
    class Output;

    // Each holds the connection until it ends
    static Rooftree::Detached ReadLoop(std::shared_ptr<Connection> self);
    static Rooftree::Detached WriteLoop(std::shared_ptr<Connection> self);
    void Closed();

    ConsoleSocketServer& _server;
    LocalSocket _socket;
    std::string _key;
    boost::asio::streambuf _buffer;
    std::deque<std::string> _writes;
    std::shared_ptr<Output> _output;
    bool _closed = false;
};

// Sends through the connection, nothing once it is gone
class ConsoleSocketServer::Connection::Output final : public ConsoleOutput
{
public:
    Output(std::weak_ptr<Connection> connection, std::string prompt) : _connection(std::move(connection)), _prompt(std::move(prompt)) { }

    void Print(std::string_view text) override { Send({ { "out", text } }); }
    void Ask(std::string_view prompt, bool secret) override { Send({ { "ask", prompt }, { "secret", secret } }); }
    void Ready() override { Send({ { "ready", _prompt } }); }

private:
    void Send(boost::json::object message)
    {
        if (std::shared_ptr<Connection> connection = _connection.lock())
            connection->Send(std::move(message));
    }

    std::weak_ptr<Connection> _connection;
    std::string _prompt;
};

void ConsoleSocketServer::Connection::Start()
{
    _output = std::make_shared<Output>(weak_from_this(), _server._prompt);
    _output->Ready();
    ReadLoop(shared_from_this());
}

Rooftree::Detached ConsoleSocketServer::Connection::ReadLoop(std::shared_ptr<Connection> self)
{
    while (!self->_closed)
    {
        auto [error, size] = co_await Rooftree::Await<boost::system::error_code, std::size_t>([&](auto done)
        {
            boost::asio::async_read_until(self->_socket, self->_buffer, '\n', std::move(done));
        });

        // Also a line longer than MaxLineSize (the buffer is full)
        if (error)
            break;

        std::string line(boost::asio::buffers_begin(self->_buffer.data()), boost::asio::buffers_begin(self->_buffer.data()) + size - 1);
        self->_buffer.consume(size);

        boost::system::error_code parseError;
        boost::json::value message = boost::json::parse(line, parseError);
        boost::json::value const* text = message.is_object() ? message.as_object().if_contains("line") : nullptr;
        if (parseError || !text || !text->is_string())
        {
            RT_LOG_WARN("server.console", "Console {} sent something else than a line, closing it", self->_key);
            break;
        }

        self->_server._sessionMgr.Enqueue(std::make_unique<ConsoleMessage>(self->_server._type, self->_key, self->_output,
            self->_server._commands, std::string(text->as_string())));
    }

    self->Closed();
}

void ConsoleSocketServer::Connection::Send(boost::json::object message)
{
    boost::asio::post(_socket.get_executor(), [self = shared_from_this(), text = boost::json::serialize(message) + "\n"]() mutable
    {
        if (self->_closed)
            return;

        self->_writes.push_back(std::move(text));
        if (self->_writes.size() == 1)
            WriteLoop(self);
    });
}

Rooftree::Detached ConsoleSocketServer::Connection::WriteLoop(std::shared_ptr<Connection> self)
{
    // Closed() empties the queue
    while (!self->_writes.empty())
    {
        auto [error, written] = co_await Rooftree::Await<boost::system::error_code, std::size_t>([&](auto done)
        {
            boost::asio::async_write(self->_socket, boost::asio::buffer(self->_writes.front()), std::move(done));
        });

        if (error)
        {
            self->Closed();
            break;
        }

        self->_writes.pop_front();
    }
}

void ConsoleSocketServer::Connection::Close()
{
    boost::asio::post(_socket.get_executor(), [self = shared_from_this()] { self->Closed(); });
}

void ConsoleSocketServer::Connection::Closed()
{
    if (std::exchange(_closed, true))
        return;

    boost::system::error_code ignored;
    _socket.close(ignored);
    _writes.clear();
    RT_LOG_INFO("server.console", "Console {} disconnected", _key);
    _server._sessionMgr.Enqueue(std::make_unique<ConsoleMessage>(_server._type, _key, _output, _server._commands, std::string(),
        ConsoleMessage::Kind::Closed));
}

ConsoleSocketServer::ConsoleSocketServer(SessionMgr& sessionMgr, CommandTable const& commands, std::string prompt)
    : _sessionMgr(sessionMgr), _type(sessionMgr.GetMessageTypes().Register("console")), _commands(commands), _prompt(std::move(prompt))
{
}

ConsoleSocketServer::~ConsoleSocketServer()
{
    Stop();
}

bool ConsoleSocketServer::Start(Asio::IoContext& ioContext, std::string const& path, std::string& error)
{
    using boost::asio::local::stream_protocol;

    // A server listening there already, or a file left over from one that crashed
    struct stat existing{};
    if (lstat(path.c_str(), &existing) == 0)
    {
        if (!S_ISSOCK(existing.st_mode))
        {
            error = path + " exists and is no socket";
            return false;
        }

        stream_protocol::socket probe(ioContext);
        boost::system::error_code probeError;
        probe.connect(stream_protocol::endpoint(path), probeError);
        if (!probeError)
        {
            error = "another server listens on " + path;
            return false;
        }
        ::unlink(path.c_str());
    }

    try
    {
        auto acceptor = std::make_unique<stream_protocol::acceptor>(ioContext, stream_protocol::endpoint(path));
        if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)
        {
            error = path + ": chmod: " + std::strerror(errno);
            ::unlink(path.c_str());
            return false;
        }
        _acceptor = std::move(acceptor);
    }
    catch (boost::system::system_error const& bindError)
    {
        error = path + ": " + bindError.code().message();
        return false;
    }

    _path = path;
    AcceptLoop();
    return true;
}

Rooftree::Detached ConsoleSocketServer::AcceptLoop()
{
    for (;;)
    {
        auto [error, socket] = co_await Rooftree::Await<boost::system::error_code, LocalSocket>([&](auto done)
        {
            _acceptor->async_accept(boost::asio::make_strand(_acceptor->get_executor()), std::move(done));
        });

        // Stop() closed the acceptor, this server may be gone already
        if (error == boost::asio::error::operation_aborted)
            co_return;

        if (error)
            continue;

        // The file's mode keeps others out already, unless someone changes it
        ucred peer{};
        socklen_t length = sizeof(peer);
        bool allowed = getsockopt(socket.native_handle(), SOL_SOCKET, SO_PEERCRED, &peer, &length) == 0
            && (peer.uid == 0 || peer.uid == geteuid());

        std::scoped_lock lock(_connectionsMutex);
        std::erase_if(_connections, [](std::weak_ptr<Connection> const& connection) { return connection.expired(); });
        if (!allowed)
            RT_LOG_WARN("server.console", "Refused a console of uid {}, only uid {} and root may connect", peer.uid, geteuid());
        else if (_connections.size() >= MaxClients)
            RT_LOG_WARN("server.console", "Refused a console, there are {} already", MaxClients);
        else
        {
            std::string key = "<console:" + std::to_string(++_nextId) + ">";
            RT_LOG_INFO("server.console", "Console {} connected (uid {})", key, peer.uid);
            auto connection = std::make_shared<Connection>(*this, std::move(socket), std::move(key));
            _connections.push_back(connection);
            connection->Start();
        }
    }
}

void ConsoleSocketServer::Stop()
{
    if (!_acceptor)
        return;

    boost::system::error_code ignored;
    _acceptor->close(ignored);
    _acceptor = nullptr;
    ::unlink(_path.c_str());

    std::scoped_lock lock(_connectionsMutex);
    for (std::weak_ptr<Connection> const& weak : _connections)
        if (std::shared_ptr<Connection> connection = weak.lock())
            connection->Close();
    _connections.clear();
}

namespace
{
CommandTable const* CompletionTable = nullptr;
std::vector<std::string> CompletionNames;

char* NextCompletion(char const* /*text*/, int state)
{
    if (std::size_t(state) >= CompletionNames.size())
        return nullptr;
    return strdup(CompletionNames[std::size_t(state)].c_str());
}

char** CompleteLine(char const* text, int /*start*/, int end)
{
    // Commands only, never file names
    rl_attempted_completion_over = 1;
    CompletionNames = CompletionTable->Complete(std::string_view(rl_line_buffer, std::size_t(end)));
    return rl_completion_matches(text, &NextCompletion);
}

// Nothing: the input ended (Ctrl+D)
Optional<std::string> ReadLine(std::string const& prompt, bool terminal, bool remember)
{
    if (!terminal)
    {
        std::string line;
        if (!std::getline(std::cin, line))
            return {};
        return line;
    }

    char* raw = readline(prompt.c_str());
    if (!raw)
    {
        std::fputc('\n', stdout);
        return {};
    }

    std::string line = raw;
    std::free(raw);
    if (remember && line.find_first_not_of(" \t") != std::string::npos)
        add_history(line.c_str());
    return line;
}

Optional<std::string> ReadSecret(std::string const& prompt, bool terminal)
{
    termios echoing{};
    if (!terminal || tcgetattr(STDIN_FILENO, &echoing) != 0)
        return ReadLine(prompt, false, false);

    std::fwrite(prompt.data(), 1, prompt.size(), stdout);
    std::fflush(stdout);
    termios silent = echoing;
    silent.c_lflag &= ~tcflag_t(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &silent);
    Optional<std::string> line = ReadLine(prompt, false, false);
    tcsetattr(STDIN_FILENO, TCSANOW, &echoing);
    std::fputc('\n', stdout);
    return line;
}

bool SendLine(int fd, std::string const& line)
{
    std::string message = boost::json::serialize(boost::json::object{ { "line", line } }) + "\n";
    for (std::size_t sent = 0; sent < message.size();)
    {
        ssize_t written = ::send(fd, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return false;
        sent += std::size_t(written);
    }
    return true;
}
}

int RunConsoleClient(std::string const& path, CommandTable const* completion)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
    {
        std::fprintf(stderr, "%s: the path is too long for a socket\n", path.c_str());
        return 1;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0)
    {
        std::fprintf(stderr, "Could not connect to %s: %s. Is the server running, as the same user?\n", path.c_str(), std::strerror(errno));
        if (fd >= 0)
            ::close(fd);
        return 1;
    }

    bool terminal = isatty(STDIN_FILENO);
    if (terminal && completion)
    {
        CompletionTable = completion;
        rl_attempted_completion_function = &CompleteLine;
    }

    std::string buffer;
    bool inputEnded = false;
    while (!inputEnded)
    {
        std::size_t end = buffer.find('\n');
        if (end == std::string::npos)
        {
            char chunk[4096];
            ssize_t read = ::recv(fd, chunk, sizeof(chunk), 0);
            if (read < 0 && errno == EINTR)
                continue;
            if (read <= 0)
                break;
            buffer.append(chunk, std::size_t(read));
            continue;
        }

        boost::system::error_code error;
        boost::json::value value = boost::json::parse(std::string_view(buffer).substr(0, end), error);
        buffer.erase(0, end + 1);
        if (error || !value.is_object())
            continue;

        boost::json::object const& message = value.as_object();
        if (boost::json::value const* out = message.if_contains("out"); out && out->is_string())
        {
            std::fwrite(out->as_string().data(), 1, out->as_string().size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
        else if (boost::json::value const* ask = message.if_contains("ask"); ask && ask->is_string())
        {
            boost::json::value const* secret = message.if_contains("secret");
            std::string prompt(ask->as_string());
            Optional<std::string> answer = secret && secret->is_bool() && secret->as_bool() ? ReadSecret(prompt, terminal)
                : ReadLine(prompt, terminal, false);
            inputEnded = !answer || !SendLine(fd, *answer);
        }
        else if (boost::json::value const* ready = message.if_contains("ready"); ready && ready->is_string())
        {
            std::string prompt(ready->as_string());
            Optional<std::string> line;
            do
                line = ReadLine(prompt, terminal, true);
            while (line && line->find_first_not_of(" \t") == std::string::npos);
            inputEnded = !line || !SendLine(fd, *line);
        }
    }

    ::close(fd);
    if (!inputEnded)
    {
        std::fprintf(stderr, "The server closed the console\n");
        return 1;
    }
    return 0;
}
}
