/*
 * The console without a terminal: CommandTable's lookup (abbreviations, ambiguous and unknown words,
 * commands with both subcommands and a handler, help) and completion, argument splitting, ConsoleSession
 * running one command at a time (asynchronous work and questions included, a client going away), the
 * unix socket server with a client speaking its protocol, and UpdateLoop's scheduled stop.
 */

#include "rooftree_catch2.h"

#include "ConsoleSocket.h"
#include "IoContext.h"
#include "SessionMgr.h"
#include "UpdateLoop.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/write.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <filesystem>
#include <thread>
#include <poll.h>
#include <unistd.h>

using namespace Rooftree::Server;
using namespace std::chrono_literals;

namespace
{
class ConsoleTestMgr final : public SessionMgr
{
public:
    ConsoleTestMgr() : SessionMgr(1) { }

protected:
    std::unique_ptr<Session> CreateSession(Message const& firstMessage) override
    {
        return std::make_unique<ConsoleSession>(*this, firstMessage.GetSessionKey(), firstMessage.GetRemoteAddress());
    }
};

// A session to hand the handlers, it is never updated
class IdleMessage final : public Message
{
public:
    explicit IdleMessage(MessageType const& type) : Message(type, "test", boost::asio::ip::make_address("127.0.0.1")) { }
    void Process(Session&) override { }
    void Reject(RejectReason) override { }
};

struct Ran
{
    std::vector<std::string> Calls;     // "<command>:<args>"
};

CommandTable MakeTable(Ran& ran, bool& result)
{
    auto record = [&ran, &result](std::string name)
    {
        return [&ran, &result, name](CommandHandler&, std::string_view args)
        {
            ran.Calls.push_back(name + ":" + std::string(args));
            return result;
        };
    };

    return CommandTable(
    {
        {
            .Name = "account",
            .Subcommands =
            {
                { .Name = "set", .Subcommands =
                    {
                        { .Name = "password", .Help = "Syntax: account set password", .Handler = record("password") },
                        { .Name = "2fa", .Handler = record("2fa") },
                    },
                },
                { .Name = "search", .Handler = record("search") },
                { .Name = "delete", .Handler = record("delete") },
            },
        },
        // A handler and subcommands: a word that is no subcommand is an argument
        { .Name = "server", .Help = "Syntax: server", .Handler = record("server"), .Subcommands = { { .Name = "info", .Handler = record("info") } } },
    });
}

// What a console printed, in order: "out:...", "ask:...", "ready"
class RecordingOutput final : public ConsoleOutput
{
public:
    std::vector<std::string> Events;

    void Print(std::string_view text) override { Events.push_back("out:" + std::string(text)); }
    void Ask(std::string_view prompt, bool secret) override { Events.push_back(std::string(secret ? "secret:" : "ask:") + std::string(prompt)); }
    void Ready() override { Events.push_back("ready"); }
};

void UpdateUntil(SessionMgr& mgr, std::function<bool()> done)
{
    for (int i = 0; i < 2000 && !done(); ++i)
    {
        mgr.Update(1);
        std::this_thread::sleep_for(1ms);
    }
}

struct Output
{
    std::vector<std::string> Lines;
    std::string All() const
    {
        std::string all;
        for (std::string const& line : Lines)
            all += line + "\n";
        return all;
    }
};
}

TEST_CASE("CommandTable finds commands by unique prefixes", "[server][console]")
{
    ConsoleTestMgr mgr;
    mgr.Enqueue(std::make_unique<IdleMessage>(mgr.GetMessageTypes().Register("test")));
    mgr.Update(1);
    Session& session = *mgr.FindSession("test");

    Ran ran;
    bool result = true;
    CommandTable table = MakeTable(ran, result);
    Output output;
    auto run = [&](std::string_view line)
    {
        output.Lines.clear();
        CommandHandler handler(session, [&](std::string_view text) { output.Lines.emplace_back(text); });
        return table.Execute(handler, line);
    };

    CHECK(run("account set password me a b") == "account set password");
    CHECK(run("  ACC  set   pass me ") == "account set password");
    CHECK(run("account set 2fa me off") == "account set 2fa");
    CHECK(ran.Calls == std::vector<std::string>{ "password:me a b", "password:me", "2fa:me off" });
    CHECK(output.Lines.empty());

    // "account se" could be set or search, "account d" can only be delete
    ran.Calls.clear();
    CHECK(run("account se me").empty());
    CHECK(output.All().find("ambiguous") != std::string::npos);
    CHECK(output.All().find("search") != std::string::npos);
    CHECK(run("account d me") == "account delete");

    CHECK(run("nothing").empty());
    CHECK(output.All().find("There is no command nothing") != std::string::npos);
    CHECK(run("account set nothing").empty());
    CHECK(output.All().find("account set has no subcommand nothing") != std::string::npos);
    CHECK(output.All().find("password") != std::string::npos);

    // A group without a handler lists its subcommands
    CHECK(run("account set") == "account set");
    CHECK(output.All().find("2fa") != std::string::npos);

    CHECK(run("server info") == "server info");
    CHECK(run("server now") == "server");
    CHECK(run("server") == "server");
    CHECK(ran.Calls == std::vector<std::string>{ "delete:me", "info:", "server:now", "server:" });

    // false: the help, unless the handler printed something itself
    result = false;
    run("account set password");
    CHECK(output.Lines == std::vector<std::string>{ "Syntax: account set password" });
    run("account set 2fa");
    CHECK(output.Lines == std::vector<std::string>{ "Wrong arguments" });

    CHECK(run("help") == "help");
    CHECK(output.All().find("account ...") != std::string::npos);
    CHECK(output.All().find("help") != std::string::npos);
    run("help acc set");
    CHECK(output.All().find("password") != std::string::npos);
    run("help account set password");
    CHECK(output.Lines == std::vector<std::string>{ "Syntax: account set password" });
}

TEST_CASE("Console arguments split on spaces, quotes keep them", "[server][console]")
{
    CHECK(SplitCommandArgs("") == std::vector<std::string>{});
    CHECK(SplitCommandArgs("  a  b\tc ") == std::vector<std::string>{ "a", "b", "c" });
    CHECK(SplitCommandArgs(R"(me "Aiden Yuan" "" x)") == std::vector<std::string>{ "me", "Aiden Yuan", "", "x" });
    CHECK(SplitCommandArgs(R"(a "unclosed b)") == std::vector<std::string>{ "a", "unclosed b" });
}

TEST_CASE("CommandTable completes the word being typed", "[server][console]")
{
    Ran ran;
    bool result = true;
    CommandTable table = MakeTable(ran, result);
    using Names = std::vector<std::string>;

    CHECK(table.Complete("") == Names{ "account", "server", "help" });
    CHECK(table.Complete("a") == Names{ "account" });
    CHECK(table.Complete("account ") == Names{ "set", "search", "delete" });
    CHECK(table.Complete("ACC se") == Names{ "set", "search" });
    CHECK(table.Complete("acc set p") == Names{ "password" });
    // Arguments, ambiguous or unknown words before: nothing
    CHECK(table.Complete("account set password m").empty());
    CHECK(table.Complete("account se x").empty());
    CHECK(table.Complete("nothing ").empty());
    // help takes a command
    CHECK(table.Complete("help acc") == Names{ "account" });
    CHECK(table.Complete("help account s") == Names{ "set", "search" });
}

Rooftree::Task<void> SleepOnWorker(CommandHandler& handler, std::vector<std::string>& events)
{
    co_await handler.GetSession().OnWorker([] { std::this_thread::sleep_for(20ms); });
    events.push_back("slow done");
}

TEST_CASE("The console runs one command at a time, asynchronous work and questions included", "[server][console]")
{
    std::vector<std::string> events;
    CommandTable table(
    {
        { .Name = "slow", .Handler = [&](CommandHandler& handler, std::string_view)
            {
                events.push_back("slow");
                handler.Spawn(SleepOnWorker(handler, events));
                return true;
            } },
        { .Name = "fast", .Handler = [&](CommandHandler&, std::string_view)
            {
                events.push_back("fast");
                return true;
            } },
        { .Name = "ask", .Handler = [&](CommandHandler& handler, std::string_view)
            {
                return handler.Ask("Password: ", true, [&](CommandHandler& handler, std::string answer)
                {
                    handler.Ask("Name: ", false, [&, first = answer](CommandHandler& handler, std::string second)
                    {
                        events.push_back("answers " + first + "," + second);
                        handler.SendSysMessage("thanks");
                    });
                });
            } },
    });

    ConsoleTestMgr mgr;
    MessageType const& type = mgr.GetMessageTypes().Register("console");
    auto output = std::make_shared<RecordingOutput>();
    auto send = [&](std::string line) { mgr.Enqueue(std::make_unique<ConsoleMessage>(type, "<console>", output, table, std::move(line))); };

    send("slow");
    send("fast");
    UpdateUntil(mgr, [&] { return events.size() >= 3; });
    CHECK(events == std::vector<std::string>{ "slow", "slow done", "fast" });
    CHECK(output->Events == std::vector<std::string>{ "ready", "ready" });

    // The two lines after ask answer it, the third is a command again; even an empty line answers
    events.clear();
    output->Events.clear();
    send("ask");
    send("");
    send("me");
    send("fast");
    UpdateUntil(mgr, [&] { return events.size() >= 2; });
    CHECK(events == std::vector<std::string>{ "answers ,me", "fast" });
    CHECK(output->Events == std::vector<std::string>{ "secret:Password: ", "ask:Name: ", "out:thanks", "ready", "ready" });
    mgr.Stop();
}

TEST_CASE("A console that goes away drops its question and its session", "[server][console]")
{
    bool answered = false;
    CommandTable table(
    {
        { .Name = "ask", .Handler = [&](CommandHandler& handler, std::string_view)
            {
                return handler.Ask("? ", false, [&](CommandHandler&, std::string) { answered = true; });
            } },
    });

    ConsoleTestMgr mgr;
    mgr.LoadSettings({ .SessionIdleTimeout = 1h }, {});
    MessageType const& type = mgr.GetMessageTypes().Register("console");
    auto output = std::make_shared<RecordingOutput>();
    mgr.Enqueue(std::make_unique<ConsoleMessage>(type, "<console:1>", output, table, "ask"));
    mgr.Update(1);
    CHECK(output->Events == std::vector<std::string>{ "ask:? " });
    // Waiting for its answer, however long
    mgr.LoadSettings({ .SessionIdleTimeout = 0s }, {});
    mgr.Update(1);
    CHECK(mgr.FindSession("<console:1>"));

    mgr.Enqueue(std::make_unique<ConsoleMessage>(type, "<console:1>", output, table, "", ConsoleMessage::Kind::Closed));
    mgr.Update(1);
    mgr.Update(1);
    CHECK_FALSE(answered);
    CHECK_FALSE(mgr.FindSession("<console:1>"));
    // The command ended with its question
    CHECK(output->Events.back() == "ready");
}

TEST_CASE("The console socket runs a client's commands", "[server][console]")
{
    CommandTable table(
    {
        { .Name = "echo", .Handler = [](CommandHandler& handler, std::string_view args)
            {
                handler.SendSysMessage(args);
                return true;
            } },
        { .Name = "secret", .Handler = [](CommandHandler& handler, std::string_view)
            {
                return handler.Ask("Password: ", true, [](CommandHandler& handler, std::string answer)
                {
                    handler.PSendSysMessage("{} characters", answer.size());
                });
            } },
    });

    std::string path = (std::filesystem::temp_directory_path() / ("rooftree-console-test-" + std::to_string(getpid()) + ".sock")).string();
    ConsoleTestMgr mgr;
    Rooftree::Asio::IoContext ioContext;
    auto workGuard = boost::asio::make_work_guard(ioContext);
    std::thread ioThread([&] { ioContext.run(); });
    ConsoleSocketServer server(mgr, table, "> ");
    std::string error;
    REQUIRE(server.Start(ioContext, path, error));
    // Only for its owner
    CHECK((std::filesystem::status(path).permissions() & std::filesystem::perms::all) == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
    // A second server on the same socket is refused
    {
        ConsoleSocketServer second(mgr, table);
        std::string secondError;
        CHECK_FALSE(second.Start(ioContext, path, secondError));
        CHECK(secondError.find("another server") != std::string::npos);
    }

    boost::asio::io_context clientContext;
    boost::asio::local::stream_protocol::socket client(clientContext);
    client.connect(boost::asio::local::stream_protocol::endpoint(path));
    std::string received;
    auto next = [&]() -> boost::json::object
    {
        for (int i = 0; i < 2000; ++i)
        {
            if (std::size_t end = received.find('\n'); end != std::string::npos)
            {
                boost::json::value value = boost::json::parse(received.substr(0, end));
                received.erase(0, end + 1);
                return value.as_object();
            }
            mgr.Update(1);
            if (std::size_t available = client.available())
            {
                std::string chunk(available, '\0');
                client.read_some(boost::asio::buffer(chunk));
                received += chunk;
            }
            else
                std::this_thread::sleep_for(1ms);
        }
        FAIL("no message from the server");
        return {};
    };
    auto send = [&](std::string line)
    {
        boost::asio::write(client, boost::asio::buffer(boost::json::serialize(boost::json::object{ { "line", line } }) + "\n"));
    };

    CHECK(next() == boost::json::object{ { "ready", "> " } });
    send("echo hello \"world\"");
    CHECK(next() == boost::json::object{ { "out", "hello \"world\"" } });
    CHECK(next() == boost::json::object{ { "ready", "> " } });
    send("secret");
    CHECK(next() == boost::json::object{ { "ask", "Password: " }, { "secret", true } });
    send("12345");
    CHECK(next() == boost::json::object{ { "out", "5 characters" } });
    CHECK(next() == boost::json::object{ { "ready", "> " } });

    // Something else than a line closes the connection
    boost::asio::write(client, boost::asio::buffer(std::string("not json\n")));
    boost::system::error_code readError;
    for (int i = 0; i < 2000 && !readError; ++i)
    {
        char byte;
        if (client.available())
            client.read_some(boost::asio::buffer(&byte, 1), readError);
        else
        {
            pollfd fd{ .fd = client.native_handle(), .events = POLLIN, .revents = 0 };
            if (poll(&fd, 1, 1) > 0)
                client.read_some(boost::asio::buffer(&byte, 1), readError);
        }
        mgr.Update(1);
    }
    CHECK(readError == boost::asio::error::eof);
    UpdateUntil(mgr, [&] { return mgr.GetSessionCount() == 0; });
    CHECK(mgr.GetSessionCount() == 0);

    workGuard.reset();
    ioContext.stop();
    ioThread.join();
    server.Stop();
    CHECK_FALSE(std::filesystem::exists(path));
    mgr.Stop();
}

TEST_CASE("UpdateLoop stops when a scheduled stop is due, with its exit code", "[server]")
{
    UpdateLoop loop(1ms);
    int updates = 0;
    loop.StopAfter(1h, 2);
    CHECK(loop.CancelScheduledStop());
    CHECK_FALSE(loop.CancelScheduledStop());

    loop.StopAfter(30ms, 2);
    CHECK(loop.GetScheduledStop());
    TimePoint start = TimePoint::clock::now();
    loop.Run([&](uint32) { ++updates; });
    CHECK(TimePoint::clock::now() - start >= 30ms);
    CHECK(updates > 0);
    CHECK(loop.GetExitCode() == 2);
    CHECK_FALSE(loop.GetScheduledStop());
}
