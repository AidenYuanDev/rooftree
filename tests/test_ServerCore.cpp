/*
 * rooftree-server's session core without the network: SessionMgr's per-session and per-update limits, the
 * order sessions are served in, AntiDOS policies, bounded queues, OnWorker, idle removal, and the
 * update loop. Plus the small helpers: RateLimiter, tokens and encodings, CSRF checks, mail building.
 */

#include "rooftree_catch2.h"

#include "Appender.h"
#include "Config.h"
#include "HttpClient.h"
#include "Log.h"
#include "LogMessage.h"
#include "Mailer.h"
#include "RateLimiter.h"
#include "SecurityUtils.h"
#include "SessionMgr.h"
#include "Task.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include "FixedTicker.h"
#include "UpdateLoop.h"
#include <thread>

using namespace Rooftree::Server;
using namespace std::chrono_literals;

namespace
{
struct MessageLog
{
    std::vector<std::string> Processed;     // "<key>:<n>"
    std::vector<std::pair<std::string, RejectReason>> Rejected;
};

class TestMessage final : public Message
{
public:
    TestMessage(MessageType const& type, std::string key, int number, MessageLog& log, char const* address = "127.0.0.1")
        : Message(type, std::move(key), boost::asio::ip::make_address(address)), _number(number), _log(log) { }

    void Process(Session& session) override { _log.Processed.push_back(session.GetKey() + ":" + std::to_string(_number)); }
    void Reject(RejectReason reason) override { _log.Rejected.emplace_back(GetSessionKey() + ":" + std::to_string(_number), reason); }

private:
    int _number;
    MessageLog& _log;
};

class ThrowingMessage final : public Message
{
public:
    ThrowingMessage(MessageType const& type, std::string key) : Message(type, std::move(key), boost::asio::ip::make_address("127.0.0.1")) { }

    void Process(Session&) override { throw std::runtime_error("a bug"); }
    void Reject(RejectReason) override { }
};

Rooftree::Task<void> WaitOnWorker(Session& session, std::atomic<bool>& release)
{
    co_await session.OnWorker([&release] { while (!release) std::this_thread::sleep_for(1ms); });
}

// Starts work on a worker that waits for Release, logged as processed when it starts
class WorkMessage final : public Message
{
public:
    WorkMessage(MessageType const& type, std::string key, int number, MessageLog& log, std::atomic<bool>& release)
        : Message(type, std::move(key), boost::asio::ip::make_address("203.0.113.1")), _number(number), _log(log), _release(release) { }

    void Process(Session& session) override
    {
        _log.Processed.push_back(session.GetKey() + ":" + std::to_string(_number));
        session.Spawn(WaitOnWorker(session, _release));
    }

    void Reject(RejectReason reason) override { _log.Rejected.emplace_back(GetSessionKey() + ":" + std::to_string(_number), reason); }

private:
    int _number;
    MessageLog& _log;
    std::atomic<bool>& _release;
};

class TestSession final : public Session
{
public:
    using Session::Session;

    bool Ready = true;
    bool Removable = true;
    int Violations = 0;
    bool* RemovedFlag = nullptr;

protected:
    bool CanProcessMessages() const override { return Ready; }
    bool CanBeRemoved() const override { return Removable; }
    void OnAntiDosViolation(Message const&, AntiDosPolicy) override { ++Violations; }
    void OnRemoved() override
    {
        if (RemovedFlag)
            *RemovedFlag = true;
    }
};

class TestSessionMgr final : public SessionMgr
{
public:
    TestSessionMgr() : SessionMgr(1) { }

    std::vector<std::string> Created;

protected:
    std::unique_ptr<Session> CreateSession(Message const& firstMessage) override
    {
        Created.push_back(firstMessage.GetSessionKey());
        return std::make_unique<TestSession>(*this, firstMessage.GetSessionKey(), firstMessage.GetRemoteAddress());
    }
};

TestSession& GetSession(SessionMgr& mgr, std::string_view key)
{
    Session* session = mgr.FindSession(key);
    REQUIRE(session);
    return static_cast<TestSession&>(*session);
}

// What server.antidos logs while one lives
class AntiDosLogCapture
{
public:
    AntiDosLogCapture()
    {
        static std::once_flag registered;
        std::call_once(registered, [] { sLog->RegisterAppender<CaptureAppender>(); });
        LoadLogConfig(R"(
            Appender:
              Capture: "10,3,0"
            Logger:
              root: "3,"
              server.antidos: "3,Capture"
        )");
        Lines().clear();
    }

    ~AntiDosLogCapture() { LoadLogConfig("Logger: { root: \"3,\" }"); }

    static std::vector<std::string>& Lines()
    {
        static std::vector<std::string> lines;
        return lines;
    }

    // The lines that start with prefix
    static std::vector<std::string> Find(std::string_view prefix)
    {
        std::vector<std::string> found;
        for (std::string const& line : Lines())
            if (line.starts_with(prefix))
                found.push_back(line);
        return found;
    }

private:
    class CaptureAppender final : public Appender
    {
    public:
        static constexpr AppenderType Type = AppenderType(10);

        CaptureAppender(uint8 id, std::string name, LogLevel level, AppenderFlags flags, std::vector<std::string_view> const& /*args*/)
            : Appender(id, std::move(name), level, flags) { }

        AppenderType GetType() const override { return Type; }

    private:
        void Write(LogMessage const& message, std::string const& /*prefix*/) override { Lines().push_back(message.Text); }
    };

    static void LoadLogConfig(std::string const& config)
    {
        std::string error;
        REQUIRE(sConfigMgr->LoadString(config, error));
        sLog->LoadFromConfig();
    }
};
}

TEST_CASE("SessionMgr runs at most N messages per session per update", "[server]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& type = mgr.GetMessageTypes().Register("test");
    mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 3 }, {});

    for (int i = 0; i < 7; ++i)
        REQUIRE(mgr.Enqueue(std::make_unique<TestMessage>(type, "a", i, log)));
    mgr.Enqueue(std::make_unique<TestMessage>(type, "b", 0, log));

    mgr.Update(1);
    CHECK(mgr.Created == std::vector<std::string>{ "a", "b" });
    CHECK(log.Processed == std::vector<std::string>{ "a:0", "a:1", "a:2", "b:0" });

    mgr.Update(1);
    mgr.Update(1);
    CHECK(log.Processed.size() == 8);
    CHECK(log.Processed.back() == "a:6");
    CHECK(mgr.GetStats().ProcessedMessages == 8);
    CHECK(log.Rejected.empty());
}

TEST_CASE("SessionMgr's per-update budget serves sessions in turn", "[server]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& type = mgr.GetMessageTypes().Register("test");
    mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 2, .MaxMessagesPerUpdate = 3 }, {});

    for (std::string key : { "a", "b", "c" })
        for (int i = 0; i < 2; ++i)
            mgr.Enqueue(std::make_unique<TestMessage>(type, key, i, log));

    mgr.Update(1);
    CHECK(log.Processed == std::vector<std::string>{ "a:0", "a:1", "b:0" });

    // b ran out of budget, the next update starts after it: c, then a (empty), then b
    mgr.Update(1);
    CHECK(log.Processed == std::vector<std::string>{ "a:0", "a:1", "b:0", "c:0", "c:1", "b:1" });
}

TEST_CASE("AntiDOS limits message types per session", "[server][antidos]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& limited = mgr.GetMessageTypes().Register("limited");
    MessageType const& free = mgr.GetMessageTypes().Register("free");
    AntiDosSettings antiDos;
    antiDos.Limits["limited"] = { .Window = 1h, .MaxCount = 2 };

    SECTION("Reject")
    {
        antiDos.Policy = AntiDosPolicy::Reject;
        mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 10 }, antiDos);
        for (int i = 0; i < 4; ++i)
            mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", i, log));
        mgr.Enqueue(std::make_unique<TestMessage>(free, "a", 9, log));
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "b", 0, log));
        mgr.Update(1);

        CHECK(log.Processed == std::vector<std::string>{ "a:0", "a:1", "a:9", "b:0" });
        REQUIRE(log.Rejected.size() == 2);
        CHECK(log.Rejected[0] == std::pair<std::string, RejectReason>("a:2", RejectReason::RateLimited));
        CHECK(GetSession(mgr, "a").Violations == 2);
        CHECK(mgr.GetStats().AntiDosViolations == 2);
    }

    SECTION("Log")
    {
        antiDos.Policy = AntiDosPolicy::Log;
        mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 10 }, antiDos);
        for (int i = 0; i < 4; ++i)
            mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", i, log));
        mgr.Update(1);

        CHECK(log.Processed.size() == 4);
        CHECK(log.Rejected.empty());
        CHECK(GetSession(mgr, "a").Violations == 2);
    }

    SECTION("Kick")
    {
        antiDos.Policy = AntiDosPolicy::Kick;
        antiDos.KickDuration = 1h;
        mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 10 }, antiDos);
        for (int i = 0; i < 4; ++i)
            mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", i, log));
        mgr.Update(1);

        CHECK(log.Processed == std::vector<std::string>{ "a:0", "a:1" });
        // a:2 over the limit, a:3 dropped with the session
        REQUIRE(log.Rejected.size() == 2);
        CHECK(log.Rejected[0].second == RejectReason::RateLimited);
        CHECK(log.Rejected[1].second == RejectReason::SessionClosed);
        CHECK_FALSE(mgr.FindSession("a"));

        // The key stays refused, no new session is created for it
        mgr.Enqueue(std::make_unique<TestMessage>(free, "a", 5, log));
        mgr.Update(1);
        CHECK(log.Rejected.back() == std::pair<std::string, RejectReason>("a:5", RejectReason::RateLimited));
        CHECK(mgr.Created.size() == 1);
    }
}

TEST_CASE("AntiDOS never counts the exempt addresses", "[server][antidos]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& limited = mgr.GetMessageTypes().Register("limited");
    AntiDosSettings antiDos;
    antiDos.Limits["limited"] = { .Window = 1h, .MaxCount = 1 };
    antiDos.Exempt = { boost::asio::ip::make_address("127.0.0.1"), boost::asio::ip::make_address("::1") };
    mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 10 }, antiDos);

    for (int i = 0; i < 3; ++i)
    {
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "host", i, log));
        // An IPv4 client of a dual stack listener
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "mapped", i, log, "::ffff:127.0.0.1"));
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "other", i, log, "203.0.113.7"));
    }
    mgr.Update(1);

    CHECK(log.Processed == std::vector<std::string>{ "host:0", "host:1", "host:2", "mapped:0", "mapped:1", "mapped:2", "other:0" });
    CHECK(log.Rejected.size() == 2);
}

TEST_CASE("AddressBlocklist blocks an address, an IPv6 one by its /64, until it ends", "[server][antidos]")
{
    using boost::asio::ip::make_address;
    Rooftree::Net::AddressBlocklist blocklist;
    TimePoint now = TimePoint::clock::now();
    CHECK_FALSE(blocklist.IsBlocked(make_address("203.0.113.7"), now));

    blocklist.Block(make_address("203.0.113.7"), now + 1h);
    blocklist.Block(make_address("2001:db8:1:2::5"), now + 1min);
    CHECK(blocklist.IsBlocked(make_address("203.0.113.7"), now));
    CHECK(blocklist.IsBlocked(make_address("::ffff:203.0.113.7"), now));
    CHECK_FALSE(blocklist.IsBlocked(make_address("203.0.113.8"), now));
    CHECK(blocklist.IsBlocked(make_address("2001:db8:1:2:ffff::1"), now));
    CHECK_FALSE(blocklist.IsBlocked(make_address("2001:db8:1:3::5"), now));

    // A shorter block doesn't cut a longer one short
    blocklist.Block(make_address("203.0.113.7"), now + 1s);
    CHECK(blocklist.IsBlocked(make_address("203.0.113.7"), now + 1min));

    CHECK_FALSE(blocklist.IsBlocked(make_address("2001:db8:1:2::5"), now + 2min));
    blocklist.Prune(now + 2min);
    CHECK(blocklist.GetCount() == 1);
}

TEST_CASE("AntiDOS blocks the address of a session refused too often", "[server][antidos]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& limited = mgr.GetMessageTypes().Register("limited");
    AntiDosSettings antiDos;
    antiDos.Limits["limited"] = { .Window = 1h, .MaxCount = 1 };
    antiDos.KickDuration = 1h;
    antiDos.BlockAfterRejections = 3;
    antiDos.BlockAfterWindow = 1h;
    antiDos.Exempt = { boost::asio::ip::make_address("127.0.0.1") };
    mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 10 }, antiDos);

    for (int i = 0; i < 5; ++i)
    {
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", i, log, "2001:db8:1:2::5"));
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "host", i, log));
    }
    mgr.Update(1);

    // The third rejection blocks it: the session closes, what it still had queued is dropped
    CHECK(log.Processed == std::vector<std::string>{ "a:0", "host:0", "host:1", "host:2", "host:3", "host:4" });
    CHECK(log.Rejected == std::vector<std::pair<std::string, RejectReason>>{ { "a:1", RejectReason::RateLimited },
        { "a:2", RejectReason::RateLimited }, { "a:3", RejectReason::RateLimited }, { "a:4", RejectReason::SessionClosed } });
    CHECK(mgr.GetAddressBlocklist()->IsBlocked(boost::asio::ip::make_address("2001:db8:1:2::5"), TimePoint::clock::now()));

    // Anything from its /64 is dropped before it reaches a session; others and the exempt host are not
    log = {};
    CHECK_FALSE(mgr.Enqueue(std::make_unique<TestMessage>(limited, "b", 0, log, "2001:db8:1:2::9")));
    CHECK(mgr.Enqueue(std::make_unique<TestMessage>(limited, "c", 0, log, "2001:db8:1:3::9")));
    CHECK(mgr.Enqueue(std::make_unique<TestMessage>(limited, "host", 5, log)));
    mgr.Update(1);
    CHECK(log.Rejected.front() == std::pair<std::string, RejectReason>("b:0", RejectReason::Blocked));
    CHECK(log.Processed == std::vector<std::string>{ "host:5", "c:0" });
    CHECK_FALSE(mgr.GetAddressBlocklist()->IsBlocked(boost::asio::ip::make_address("127.0.0.1"), TimePoint::clock::now()));
}

TEST_CASE("AntiDOS limits may be loaded before their types are registered", "[server][antidos]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    AntiDosSettings antiDos;
    antiDos.Limits["limited"] = { .Window = 1h, .MaxCount = 2 };
    // Like an app: settings first, then the routes that register the types
    mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 10 }, antiDos);
    MessageType const& limited = mgr.GetMessageTypes().Register("limited");

    for (int i = 0; i < 3; ++i)
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", i, log));
    mgr.Update(1);

    CHECK(log.Processed == std::vector<std::string>{ "a:0", "a:1" });
    CHECK(log.Rejected == std::vector<std::pair<std::string, RejectReason>>{ { "a:2", RejectReason::RateLimited } });
}

TEST_CASE("AntiDOS logs a run of violations when it starts and when it ends", "[server][antidos]")
{
    AntiDosLogCapture capture;
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& limited = mgr.GetMessageTypes().Register("limited");
    AntiDosSettings antiDos;
    antiDos.Limits["limited"] = { .Window = 100ms, .MaxCount = 1 };
    mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 10, .AntiDosLogInterval = 1h }, antiDos);

    for (int i = 0; i < 5; ++i)
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", i, log));
    mgr.Update(1);
    CHECK(log.Rejected.size() == 4);
    // The hook still runs for every one of them
    CHECK(GetSession(mgr, "a").Violations == 4);
    CHECK(capture.Lines() == std::vector<std::string>{
        "AntiDOS: session a (127.0.0.1) is over the limit of limited (1 per 100 ms), rejecting; more are counted" });

    // A whole window without one ends the run
    std::this_thread::sleep_for(150ms);
    mgr.Update(1);
    REQUIRE(capture.Lines().size() == 2);
    CHECK(capture.Lines()[1] == "AntiDOS: session a limited: 3 more rejected in 0 ms, under the limit again");

    // The next violation starts a new one
    for (int i = 5; i < 7; ++i)
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", i, log));
    mgr.Update(1);
    REQUIRE(capture.Lines().size() == 3);
    CHECK(capture.Lines()[2].ends_with("is over the limit of limited (1 per 100 ms), rejecting; more are counted"));

    // A single violation says it all in its first line
    std::this_thread::sleep_for(150ms);
    mgr.Update(1);
    CHECK(capture.Lines().size() == 3);
}

TEST_CASE("AntiDOS logs a long run once per interval", "[server][antidos]")
{
    AntiDosLogCapture capture;
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& limited = mgr.GetMessageTypes().Register("limited");
    AntiDosSettings antiDos;
    antiDos.Limits["limited"] = { .Window = 1h, .MaxCount = 1 };
    mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 10, .AntiDosLogInterval = 50ms }, antiDos);

    for (int i = 0; i < 2; ++i)
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", i, log));
    mgr.Update(1);
    CHECK(capture.Find("AntiDOS: session a").size() == 1);

    std::this_thread::sleep_for(60ms);
    mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", 2, log));
    mgr.Update(1);
    std::vector<std::string> lines = capture.Find("AntiDOS: session a");
    REQUIRE(lines.size() == 2);
    CHECK(lines[1].starts_with("AntiDOS: session a limited: 1 more rejected in "));
    CHECK(lines[1].ends_with(" ms, still over"));

    // Within the interval nothing more is logged, until the session goes away
    mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", 3, log));
    mgr.Update(1);
    CHECK(capture.Find("AntiDOS: session a").size() == 2);
    GetSession(mgr, "a").Close();
    mgr.Update(1);
    lines = capture.Find("AntiDOS: session a");
    REQUIRE(lines.size() == 3);
    CHECK(lines[2] == "AntiDOS: session a limited: 1 more rejected in 0 ms, session removed");
}

TEST_CASE("Rejections are logged as a summary per interval", "[server][antidos]")
{
    AntiDosLogCapture capture;
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& limited = mgr.GetMessageTypes().Register("limited");
    AntiDosSettings antiDos;
    antiDos.Limits["limited"] = { .Window = 1h, .MaxCount = 1 };
    mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 10, .MaxSessions = 1, .AntiDosLogInterval = 100ms }, antiDos);

    for (int i = 0; i < 3; ++i)
        mgr.Enqueue(std::make_unique<TestMessage>(limited, "a", i, log));
    // No room for a second session
    mgr.Enqueue(std::make_unique<TestMessage>(limited, "b", 0, log));
    mgr.Update(1);
    CHECK(capture.Find("AntiDOS: last").empty());

    std::this_thread::sleep_for(110ms);
    mgr.Update(1);
    std::vector<std::string> summaries = capture.Find("AntiDOS: last");
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].ends_with(": 3 rejected (over a limit 2, server busy 1), 1 sessions over a limit, 1/1 sessions (full)"));

    // Nothing rejected, nothing to say
    std::this_thread::sleep_for(110ms);
    mgr.Update(1);
    CHECK(capture.Find("AntiDOS: last").size() == 1);
}

TEST_CASE("AntiDOS settings are read from the config", "[server][antidos]")
{
    std::shared_ptr<Config const> config = Config::Parse(YAML::Load(R"(
AntiDos:
  Policy: Kick
  KickDuration: 30
  Limits:
    comment.create: { Window: 60, MaxCount: 3 }
    auth.login: { Window: 60, MaxCount: 5 }
Bad:
  Limits:
    x: { Window: 0, MaxCount: 1 }
)"), "");
    REQUIRE(config);

    Optional<AntiDosSettings> settings = AntiDosSettings::FromConfig(*config, "AntiDos");
    REQUIRE(settings.has_value());
    CHECK(settings->Policy == AntiDosPolicy::Kick);
    CHECK(settings->KickDuration == 30s);
    REQUIRE(settings->Limits.contains("comment.create"));
    CHECK(settings->Limits["comment.create"].MaxCount == 3);
    CHECK(settings->Limits["comment.create"].Window == 60s);

    CHECK(settings->Exempt.empty());

    CHECK_FALSE(AntiDosSettings::FromConfig(*config, "Bad").has_value());
    CHECK(AntiDosSettings::FromConfig(*config, "Missing")->Limits.empty());
}

TEST_CASE("AntiDOS settings from the config over an app's defaults", "[server][antidos]")
{
    std::shared_ptr<Config const> config = Config::Parse(YAML::Load(R"(
AntiDos:
  Exempt: ["::1"]
  BlockAfter: { Window: 10, Rejections: 1000 }
  Limits:
    auth.login: { Window: 60, MaxCount: 5 }
    posts.read: { Window: 10, MaxCount: 0 }
BadExempt:
  Exempt: ["localhost"]
BadBlock:
  BlockAfter: { Rejections: 5 }
)"), "");
    REQUIRE(config);

    AntiDosSettings defaults;
    defaults.Policy = AntiDosPolicy::Kick;
    defaults.Exempt = { boost::asio::ip::make_address("127.0.0.1") };
    defaults.Limits["auth.login"] = { .Window = 60s, .MaxCount = 10 };
    defaults.Limits["posts.read"] = { .Window = 10s, .MaxCount = 200 };
    defaults.Limits["comments.read"] = { .Window = 10s, .MaxCount = 200 };

    Optional<AntiDosSettings> settings = AntiDosSettings::FromConfig(*config, "AntiDos", defaults);
    REQUIRE(settings.has_value());
    CHECK(settings->Policy == AntiDosPolicy::Kick);
    CHECK(settings->Exempt == std::vector{ boost::asio::ip::make_address("::1") });
    CHECK(settings->BlockAfterRejections == 1000);
    CHECK(settings->BlockAfterWindow == 10s);
    CHECK(settings->Limits["auth.login"].MaxCount == 5);
    // MaxCount 0 lifts a default
    CHECK(settings->Limits["posts.read"].MaxCount == 0);
    // Not named: the default stays
    CHECK(settings->Limits["comments.read"].MaxCount == 200);

    // Nothing in the config at all: the defaults
    Optional<AntiDosSettings> missing = AntiDosSettings::FromConfig(*config, "Missing", defaults);
    REQUIRE(missing.has_value());
    CHECK(missing->Exempt == defaults.Exempt);
    CHECK(missing->Limits.size() == 3);

    CHECK_FALSE(AntiDosSettings::FromConfig(*config, "BadExempt", defaults).has_value());
    CHECK_FALSE(AntiDosSettings::FromConfig(*config, "BadBlock", defaults).has_value());
}

TEST_CASE("SessionMgr bounds its queues", "[server]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& type = mgr.GetMessageTypes().Register("test");

    SECTION("per session")
    {
        mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 1, .MaxQueuedMessagesPerSession = 2 }, {});
        for (int i = 0; i < 4; ++i)
            mgr.Enqueue(std::make_unique<TestMessage>(type, "a", i, log));
        mgr.Update(1);

        CHECK(log.Processed == std::vector<std::string>{ "a:0" });
        REQUIRE(log.Rejected.size() == 2);
        CHECK(log.Rejected[0] == std::pair<std::string, RejectReason>("a:2", RejectReason::Overloaded));
    }

    SECTION("inbox")
    {
        mgr.LoadSettings({ .MaxPendingMessages = 2 }, {});
        CHECK(mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 0, log)));
        CHECK(mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 1, log)));
        CHECK_FALSE(mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 2, log)));
        CHECK(log.Rejected == std::vector<std::pair<std::string, RejectReason>>{ { "a:2", RejectReason::Overloaded } });

        mgr.Update(1);
        CHECK(mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 3, log)));
    }
    SECTION("sessions")
    {
        mgr.LoadSettings({ .MaxSessions = 2, .SessionIdleTimeout = 1h }, {});
        for (std::string key : { "a", "b", "c" })
            mgr.Enqueue(std::make_unique<TestMessage>(type, key, 0, log));
        mgr.Update(1);

        CHECK(log.Processed == std::vector<std::string>{ "a:0", "b:0" });
        CHECK(log.Rejected == std::vector<std::pair<std::string, RejectReason>>{ { "c:0", RejectReason::Overloaded } });

        // An existing session still gets its messages, a new key once there is room again
        GetSession(mgr, "a").Close();
        mgr.Enqueue(std::make_unique<TestMessage>(type, "b", 1, log));
        mgr.Update(1);
        mgr.Enqueue(std::make_unique<TestMessage>(type, "c", 1, log));
        mgr.Update(1);
        CHECK(log.Processed == std::vector<std::string>{ "a:0", "b:0", "b:1", "c:1" });
    }
    SECTION("idle sessions go sooner while the table is busy")
    {
        mgr.LoadSettings({ .MaxSessions = 4, .SessionIdleTimeout = 1h, .SessionIdleTimeoutWhenBusy = 0s }, {});
        for (std::string key : { "a", "b" })
            mgr.Enqueue(std::make_unique<TestMessage>(type, key, 0, log));
        mgr.Update(1);
        // Half full: an hour still
        CHECK(mgr.GetSessionCount() == 2);

        mgr.Enqueue(std::make_unique<TestMessage>(type, "c", 0, log));
        mgr.Update(1);
        // Three quarters: every idle one goes
        CHECK(mgr.GetSessionCount() == 0);
        CHECK(log.Processed == std::vector<std::string>{ "a:0", "b:0", "c:0" });
    }
}

TEST_CASE("Sessions hold messages back, close and expire", "[server]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& type = mgr.GetMessageTypes().Register("test");
    mgr.LoadSettings({ .SessionIdleTimeout = 0s }, {});

    mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 0, log));
    mgr.Update(1);
    // Idle right after its message: removed in the same update
    CHECK_FALSE(mgr.FindSession("a"));

    mgr.LoadSettings({ .SessionIdleTimeout = 1h }, {});
    mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 1, log));
    mgr.Update(1);
    TestSession& session = GetSession(mgr, "a");
    session.Ready = false;
    mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 2, log));
    mgr.Update(1);
    CHECK(log.Processed.size() == 2);
    CHECK(session.GetQueuedMessageCount() == 1);

    session.Ready = true;
    mgr.Update(1);
    CHECK(log.Processed.size() == 3);

    bool removed = false;
    session.RemovedFlag = &removed;
    mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 3, log));
    session.Ready = false;
    mgr.Update(1);
    session.Close();
    mgr.Update(1);
    CHECK(removed);
    CHECK(log.Rejected.back() == std::pair<std::string, RejectReason>("a:3", RejectReason::SessionClosed));
}

TEST_CASE("Sessions removed together keep the serving order", "[server]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& type = mgr.GetMessageTypes().Register("test");
    mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 1, .MaxMessagesPerUpdate = 4, .SessionIdleTimeout = 1h }, {});

    for (std::string key : { "a", "b", "c", "d", "e" })
        for (int i = 0; i < 2; ++i)
            mgr.Enqueue(std::make_unique<TestMessage>(type, key, i, log));
    mgr.Update(1);
    CHECK(log.Processed == std::vector<std::string>{ "a:0", "b:0", "c:0", "d:0" });

    // e is next; a and c go, one before it and one after it in the next update's order
    GetSession(mgr, "a").Close();
    GetSession(mgr, "c").Close();
    mgr.Update(1);
    CHECK(mgr.GetSessionCount() == 3);
    CHECK_FALSE(mgr.FindSession("a"));
    CHECK(log.Processed == std::vector<std::string>{ "a:0", "b:0", "c:0", "d:0", "e:0", "b:1", "d:1" });
    CHECK(log.Rejected == std::vector<std::pair<std::string, RejectReason>>{ { "a:1", RejectReason::SessionClosed }, { "c:1", RejectReason::SessionClosed } });

    // The budget wasn't used up, so e still starts the next update
    mgr.Enqueue(std::make_unique<TestMessage>(type, "b", 2, log));
    mgr.Enqueue(std::make_unique<TestMessage>(type, "d", 2, log));
    mgr.Update(1);
    CHECK(std::vector<std::string>(log.Processed.end() - 3, log.Processed.end()) == std::vector<std::string>{ "e:1", "b:2", "d:2" });

    // A burst of addresses expiring in the same update
    mgr.LoadSettings({ .MaxSessions = 0, .SessionIdleTimeout = 1h }, {});
    for (int i = 0; i < 20000; ++i)
        mgr.Enqueue(std::make_unique<TestMessage>(type, "n" + std::to_string(i), i, log));
    mgr.Update(1);
    REQUIRE(mgr.GetSessionCount() == 20003);
    mgr.LoadSettings({ .SessionIdleTimeout = 0s }, {});
    mgr.Update(1);
    CHECK(mgr.GetSessionCount() == 0);
}

namespace
{
Rooftree::Task<int> AddOnWorker(Session& session, int a, int b)
{
    co_return co_await session.OnWorker([a, b] { return a + b; });
}

struct SetOnDestroy
{
    explicit SetOnDestroy(bool& flag) : Flag(&flag) { }
    SetOnDestroy(SetOnDestroy&& other) noexcept : Flag(std::exchange(other.Flag, nullptr)) { }
    ~SetOnDestroy()
    {
        if (Flag)
            *Flag = true;
    }
    bool* Flag;
};

// One session in mgr, kept while the test needs it
TestSession& MakeSession(TestSessionMgr& mgr, MessageLog& log)
{
    MessageType const& type = mgr.GetMessageTypes().Register("test");
    mgr.LoadSettings({ .SessionIdleTimeout = 1h }, {});
    mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 0, log));
    mgr.Update(1);
    return GetSession(mgr, "a");
}

template<typename Done>
void UpdateUntil(SessionMgr& mgr, Done const& done)
{
    for (int i = 0; i < 2000 && !done(); ++i)
    {
        mgr.Update(1);
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(done());
}
}

TEST_CASE("A Task starts when awaited and hands over its value or exception", "[server][coroutine]")
{
    bool started = false;
    auto make = [&]() -> Rooftree::Task<std::unique_ptr<int>>
    {
        started = true;
        co_return std::make_unique<int>(7);
    };

    MessageLog log;
    TestSessionMgr mgr;
    TestSession& session = MakeSession(mgr, log);

    SECTION("nothing runs until it is awaited, destroying it unstarted is fine")
    {
        {
            auto unstarted = make();
            CHECK_FALSE(started);
        }
        CHECK_FALSE(started);
    }

    SECTION("the value, of a type that only moves")
    {
        int value = 0;
        session.Spawn([&]() -> Rooftree::Task<void>
        {
            value = *co_await make();
        }());
        CHECK(started);
        CHECK(value == 7);
    }

    SECTION("an exception comes out of the co_await")
    {
        std::string caught;
        session.Spawn([&]() -> Rooftree::Task<void>
        {
            auto throwing = []() -> Rooftree::Task<int>
            {
                throw std::runtime_error("inner");
                co_return 1;
            };
            try
            {
                co_await throwing();
            }
            catch (std::exception const& e)
            {
                caught = e.what();
            }
        }());
        CHECK(caught == "inner");
    }
}

namespace
{
Rooftree::Detached AwaitTimer(boost::asio::steady_timer& timer, std::atomic<int>& state, SetOnDestroy /*holds*/)
{
    state = 1;
    auto [error] = co_await Rooftree::Await<boost::system::error_code>([&](auto done) { timer.async_wait(std::move(done)); });
    state = error ? 3 : 2;
}
}

TEST_CASE("Await goes on in the callback, or goes away with the operation", "[server][coroutine]")
{
    bool destroyed = false;
    std::atomic<int> state = 0;

    SECTION("starts on this thread, goes on when the operation calls back, frees itself at its end")
    {
        boost::asio::io_context context;
        boost::asio::steady_timer timer(context, 1ms);
        AwaitTimer(timer, state, SetOnDestroy(destroyed));
        CHECK(state == 1);
        CHECK_FALSE(destroyed);
        context.run();
        CHECK(state == 2);
        CHECK(destroyed);
    }

    SECTION("an error comes out of the co_await")
    {
        boost::asio::io_context context;
        boost::asio::steady_timer timer(context, 1h);
        AwaitTimer(timer, state, SetOnDestroy(destroyed));
        timer.cancel();
        context.run();
        CHECK(state == 3);
        CHECK(destroyed);
    }

    SECTION("destroyed with the operation if it never calls back")
    {
        {
            boost::asio::io_context context;
            boost::asio::steady_timer timer(context, 1h);
            AwaitTimer(timer, state, SetOnDestroy(destroyed));
            context.stop();
            context.poll();
            CHECK_FALSE(destroyed);
        }
        CHECK(state == 1);
        CHECK(destroyed);
    }
}

namespace
{
// A callback API: keeps the callback until Call() or Drop()
struct LaterCallback
{
    std::function<void(int)> Pending;
    void Call(int value) { std::exchange(Pending, nullptr)(value); }
    void Drop() { Pending = nullptr; }
};

// state: 1 waiting, then 10 + what it was called with, or 2 for never
Rooftree::Task<void> WaitForCall(std::function<void(std::function<void(int)>)> initiate, int& state)
{
    state = 1;
    Optional<std::tuple<int>> called = co_await Rooftree::WhenCalled<int>([&](auto done) { initiate(done); });
    state = called ? 10 + std::get<0>(*called) : 2;
}
}

TEST_CASE("WhenCalled goes on when the callback is called, now, later or never", "[server][coroutine]")
{
    MessageLog log;
    TestSessionMgr mgr;
    TestSession& session = MakeSession(mgr, log);
    LaterCallback api;
    int state = 0;

    SECTION("called right away: no suspending")
    {
        session.Spawn(WaitForCall([](std::function<void(int)> done) { done(5); }, state));
        CHECK(state == 15);
    }

    SECTION("called later")
    {
        session.Spawn(WaitForCall([&](std::function<void(int)> done) { api.Pending = std::move(done); }, state));
        CHECK(state == 1);
        api.Call(7);
        CHECK(state == 17);
    }

    SECTION("never called: dropped right away or later")
    {
        session.Spawn(WaitForCall([](std::function<void(int)>) { }, state));
        CHECK(state == 2);

        session.Spawn(WaitForCall([&](std::function<void(int)> done) { api.Pending = std::move(done); }, state));
        CHECK(state == 1);
        api.Drop();
        CHECK(state == 2);
    }

    SECTION("a call after the coroutine was destroyed does nothing")
    {
        {
            TestSessionMgr other;
            TestSession& gone = MakeSession(other, log);
            gone.Spawn(WaitForCall([&](std::function<void(int)> done) { api.Pending = std::move(done); }, state));
            CHECK(state == 1);
        }
        api.Call(3);
        CHECK(state == 1);
    }
}

// A lambda that is a coroutine has to outlive it: its captures are in the lambda, not in the frame
TEST_CASE("Session::OnWorker works on a worker and goes on on the main thread", "[server][coroutine]")
{
    MessageLog log;
    TestSessionMgr mgr;
    TestSession& session = MakeSession(mgr, log);
    std::thread::id const mainThread = std::this_thread::get_id();

    SECTION("one after the other, through a Task awaited")
    {
        std::thread::id workerThread;
        std::thread::id afterThread;
        int result = 0;
        auto coroutine = [&]() -> Rooftree::Task<void>
        {
            co_await session.OnWorker([&] { workerThread = std::this_thread::get_id(); });
            int first = co_await AddOnWorker(session, 1, 2);
            result = co_await AddOnWorker(session, first, 39);
            afterThread = std::this_thread::get_id();
        };
        session.Spawn(coroutine());

        CHECK(result == 0);
        UpdateUntil(mgr, [&] { return result != 0; });
        CHECK(result == 42);
        CHECK(workerThread != mainThread);
        CHECK(afterThread == mainThread);
    }

    SECTION("what work throws comes out of the co_await")
    {
        std::string caught;
        auto coroutine = [&]() -> Rooftree::Task<void>
        {
            try
            {
                co_await session.OnWorker([] { throw std::runtime_error("no database"); });
            }
            catch (std::exception const& e)
            {
                caught = e.what();
            }
        };
        session.Spawn(coroutine());
        UpdateUntil(mgr, [&] { return !caught.empty(); });
        CHECK(caught == "no database");
    }

    SECTION("what a coroutine lets out is logged, the session goes on")
    {
        bool done = false;
        auto coroutine = [&]() -> Rooftree::Task<void>
        {
            co_await session.OnWorker([] { });
            done = true;
            throw std::runtime_error("a bug");
        };
        session.Spawn(coroutine());
        UpdateUntil(mgr, [&] { return done; });
        mgr.Update(1);

        int result = 0;
        auto again = [&]() -> Rooftree::Task<void> { result = co_await AddOnWorker(session, 2, 2); };
        session.Spawn(again());
        UpdateUntil(mgr, [&] { return result != 0; });
        CHECK(result == 4);
        CHECK_FALSE(session.IsClosed());
    }

    SECTION("it keeps the session and counts as work under way")
    {
        mgr.LoadSettings({ .SessionIdleTimeout = 0s }, {});
        std::atomic<bool> release = false;
        bool done = false;
        auto coroutine = [&]() -> Rooftree::Task<void>
        {
            co_await session.OnWorker([&] { while (!release) std::this_thread::sleep_for(1ms); });
            done = true;
        };
        session.Spawn(coroutine());

        for (int i = 0; i < 20; ++i)
            mgr.Update(1);
        CHECK(mgr.FindSession("a"));
        release = true;
        UpdateUntil(mgr, [&] { return done; });
        mgr.Update(1);
        CHECK_FALSE(mgr.FindSession("a"));
    }

    SECTION("a stop destroys what still waits, with what it holds")
    {
        std::atomic<bool> release = false;
        bool destroyed = false;
        bool wentOn = false;
        auto coroutine = [&](SetOnDestroy) -> Rooftree::Task<void>
        {
            co_await session.OnWorker([&] { while (!release) std::this_thread::sleep_for(1ms); });
            wentOn = true;
        };
        session.Spawn(coroutine(SetOnDestroy(destroyed)));

        CHECK_FALSE(destroyed);
        std::thread releaser([&] { std::this_thread::sleep_for(20ms); release = true; });
        mgr.Stop();
        releaser.join();
        CHECK(destroyed);
        CHECK_FALSE(wentOn);
    }
}

TEST_CASE("Session classes are served by priority, each within its own settings", "[server]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& web = mgr.GetMessageTypes().Register("web.read");
    MessageType const& move = mgr.GetMessageTypes().Register("game.move", "game");
    SessionMgrSettings settings{ .MaxMessagesPerUpdate = 3, .MaxQueuedMessagesPerSession = 2 };
    settings.Classes["game"] = { .Priority = 0, .MaxMessagesPerUpdate = 5, .MaxQueuedMessagesPerSession = 100 };
    mgr.LoadSettings(settings, {});

    // The web's first: its session is older, still the game's go first
    for (int i = 0; i < 2; ++i)
        mgr.Enqueue(std::make_unique<TestMessage>(web, "w" + std::to_string(i), 0, log));
    mgr.Update(1);
    log = {};

    for (int i = 0; i < 4; ++i)
    {
        mgr.Enqueue(std::make_unique<TestMessage>(web, "w0", i, log));
        mgr.Enqueue(std::make_unique<TestMessage>(web, "w1", i, log));
        mgr.Enqueue(std::make_unique<TestMessage>(move, "p0", i, log));
        mgr.Enqueue(std::make_unique<TestMessage>(move, "p1", i, log));
    }
    mgr.Update(1);

    CHECK(GetSession(mgr, "p0").GetSessionClass() == "game");
    CHECK(GetSession(mgr, "w0").GetSessionClass().empty());
    CHECK(mgr.GetSessionCount("game") == 2);
    CHECK(mgr.GetSessionCount("") == 2);
    // The web's queues hold 2 each, the game's all 4; the game's budget of 5 first, then the web's 3
    CHECK(log.Rejected.size() == 4);
    CHECK(log.Processed == std::vector<std::string>{ "p0:0", "p0:1", "p0:2", "p0:3", "p1:0", "w0:0", "w0:1", "w1:0" });

    // A web flood doesn't take what the game gets per update
    log = {};
    mgr.Update(1);
    CHECK(log.Processed == std::vector<std::string>{ "p1:1", "p1:2", "p1:3", "w1:1" });
}

TEST_CASE("AntiDOS blocks only the session of a class blocked by session", "[server][antidos]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& move = mgr.GetMessageTypes().Register("game.move", "game");
    AntiDosSettings antiDos;
    antiDos.Limits["game.move"] = { .Window = 1h, .MaxCount = 1 };
    antiDos.KickDuration = 1h;
    antiDos.BlockAfterRejections = 2;
    SessionMgrSettings settings;
    settings.Classes["game"] = { .Block = BlockScope::Session };
    mgr.LoadSettings(settings, antiDos);

    for (int i = 0; i < 3; ++i)
        mgr.Enqueue(std::make_unique<TestMessage>(move, "player:1", i, log, "203.0.113.7"));
    mgr.Update(1);
    CHECK_FALSE(mgr.FindSession("player:1"));

    // Another player behind the same address plays on; the cheater stays out
    log = {};
    CHECK_FALSE(mgr.GetAddressBlocklist()->IsBlocked(boost::asio::ip::make_address("203.0.113.7"), TimePoint::clock::now()));
    mgr.Enqueue(std::make_unique<TestMessage>(move, "player:2", 0, log, "203.0.113.7"));
    mgr.Enqueue(std::make_unique<TestMessage>(move, "player:1", 9, log, "203.0.113.7"));
    mgr.Update(1);
    CHECK(log.Processed == std::vector<std::string>{ "player:2:0" });
    CHECK(log.Rejected == std::vector<std::pair<std::string, RejectReason>>{ { "player:1:9", RejectReason::RateLimited } });
}

TEST_CASE("Session classes from the config, over the default class", "[server]")
{
    std::shared_ptr<Config const> config = Config::Parse(YAML::Load(R"(
Sessions:
  MaxMessagesPerUpdate: 2000
  MaxQueuedMessagesPerSession: 64
  SessionIdleTimeout: 300
  Classes:
    game:
      Priority: 0
      MaxMessagesPerUpdate: 20000
      MaxQueuedMessagesPerSession: 200
      Block: Session
)"), "");
    REQUIRE(config);

    SessionMgrSettings settings = SessionMgrSettings::FromConfig(*config, "Sessions");
    CHECK(settings.MaxMessagesPerUpdate == 2000);
    CHECK(settings.Block == BlockScope::Address);
    REQUIRE(settings.Classes.contains("game"));
    SessionClassSettings const& game = settings.Classes.at("game");
    CHECK(game.Priority == 0);
    CHECK(game.MaxMessagesPerUpdate == 20000);
    CHECK(game.MaxQueuedMessagesPerSession == 200);
    CHECK(game.Block == BlockScope::Session);
    // Not given: the default class's
    CHECK(game.SessionIdleTimeout == 300s);
    CHECK(game.MaxMessagesPerSessionPerUpdate == 100);
}

TEST_CASE("A session's messages wait while its work under way is at MaxPendingWorkPerSession", "[server]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    std::atomic<bool> release = false;
    TestSessionMgr mgr;
    MessageType const& type = mgr.GetMessageTypes().Register("test");
    mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 100, .MaxPendingWorkPerSession = 2 }, {});

    for (int i = 0; i < 4; ++i)
        mgr.Enqueue(std::make_unique<WorkMessage>(type, "a", i, log, release));
    mgr.Enqueue(std::make_unique<TestMessage>(type, "b", 0, log));
    mgr.Update(1);
    mgr.Update(1);

    // Two at work, the other two wait; another session isn't held back
    CHECK(log.Processed == std::vector<std::string>{ "a:0", "a:1", "b:0" });

    release = true;
    for (int i = 0; i < 200 && log.Processed.size() < 5; ++i)
    {
        mgr.Update(1);
        std::this_thread::sleep_for(1ms);
    }
    CHECK(log.Processed == std::vector<std::string>{ "a:0", "a:1", "b:0", "a:2", "a:3" });
    CHECK(log.Rejected.empty());
}

TEST_CASE("What throws goes alone: a message, a session's work", "[server]")
{
    // Before mgr: its destructor rejects what is still queued, into the log
    MessageLog log;
    TestSessionMgr mgr;
    MessageType const& type = mgr.GetMessageTypes().Register("test");
    mgr.LoadSettings({ .SessionIdleTimeout = 1h }, {});

    mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 0, log));
    mgr.Enqueue(std::make_unique<ThrowingMessage>(type, "a"));
    mgr.Enqueue(std::make_unique<TestMessage>(type, "a", 1, log));
    mgr.Enqueue(std::make_unique<TestMessage>(type, "b", 0, log));
    mgr.Update(1);
    CHECK(log.Processed == std::vector<std::string>{ "a:0", "a:1", "b:0" });
    CHECK(mgr.FindSession("a"));

    bool done = false;
    auto coroutine = [&]() -> Rooftree::Task<void>
    {
        co_await GetSession(mgr, "a").OnWorker([]() -> int { throw std::runtime_error("a bug on a worker"); });
        done = true;
    };
    GetSession(mgr, "a").Spawn(coroutine());
    mgr.LoadSettings({ .SessionIdleTimeout = 0s }, {});
    // Its work is over, the coroutine didn't go on, and the session can go
    for (int i = 0; i < 200 && mgr.FindSession("a"); ++i)
    {
        mgr.Update(1);
        std::this_thread::sleep_for(1ms);
    }
    CHECK_FALSE(done);
    CHECK_FALSE(mgr.FindSession("a"));
}

TEST_CASE("FixedTicker ticks at its interval, catching up a few at most", "[server]")
{
    FixedTicker ticker;
    std::vector<uint32> ticks;
    auto tick = [&ticks](uint32 interval) { ticks.push_back(interval); };

    // Off until it has an interval
    ticker.Update(1000, tick);
    CHECK(ticks.empty());

    ticker.SetInterval(50ms);
    for (int i = 0; i < 5; ++i)
        ticker.Update(20, tick);
    // At 60 and 100 ms
    CHECK(ticks == std::vector<uint32>{ 50, 50 });

    // A long update: 3 ticks, the other 7 dropped, not run later
    ticks.clear();
    ticker.Update(500, tick);
    CHECK(ticks.size() == 3);
    FixedTickerStats stats = ticker.TakeStats();
    CHECK(stats.Ticks == 5);
    CHECK(stats.Dropped == 7);
    ticks.clear();
    ticker.Update(20, tick);
    CHECK(ticks.empty());
}

TEST_CASE("UpdateLoop updates with the real diff until stopped", "[server]")
{
    UpdateLoop loop(5ms);
    loop.SetStatsInterval(0ms);
    std::vector<uint32> diffs;
    loop.Run([&](uint32 diff)
    {
        diffs.push_back(diff);
        if (diffs.size() == 5)
            loop.Stop();
    });

    REQUIRE(diffs.size() == 5);
    for (uint32 diff : diffs)
        CHECK(diff >= 5);
    CHECK(loop.GetStats().Updates == 5);
    CHECK(loop.GetLoopCounter() >= 5);
}

TEST_CASE("RateLimiter allows MaxCount per sliding window", "[server]")
{
    RateLimiter<std::string> limiter(2, 10s);
    TimePoint start = TimePoint::clock::now();

    CHECK(limiter.Allow("a", start));
    CHECK(limiter.Allow("a", start + 1s));
    CHECK_FALSE(limiter.Allow("a", start + 2s));
    CHECK(limiter.Allow("b", start + 2s));
    CHECK(limiter.GetRetryTime("a", start + 2s) == start + 10s);
    CHECK(limiter.Allow("a", start + 10s));        // the first one left the window
    CHECK_FALSE(limiter.Allow("a", start + 10s));

    limiter.Cleanup(start + 30s);
    CHECK(limiter.GetKeyCount() == 0);
}

TEST_CASE("Tokens, encodings and CSRF checks", "[server][security]")
{
    std::vector<uint8> bytes{ 'f', 'o', 'o', 'b', 'a', 'r' };
    // RFC 4648 test vectors
    CHECK(Base32Encode(std::span(bytes).first(1)) == "MY");
    CHECK(Base32Encode(bytes) == "MZXW6YTBOI");
    CHECK(Base32Decode("mzxw6ytboi") == bytes);
    CHECK_FALSE(Base32Decode("MZXW6YTBO1").has_value());
    CHECK(FormUrlEncode({ { "a b", "x&y=z" }, { "scope", "read:user user:email" }, { "ok", "A-z_0.9~" } }) == "a%20b=x%26y%3Dz&scope=read%3Auser%20user%3Aemail&ok=A-z_0.9~");
    CHECK(Base64Encode(std::vector<uint8>{ 0xFB, 0xFF }) == "+/8=");
    CHECK(Base64Encode(std::vector<uint8>{ 0x66, 0x6F, 0x6F, 0x62 }) == "Zm9vYg==");
    CHECK(Base64UrlEncode(std::vector<uint8>{ 0xFB, 0xFF }) == "-_8");
    CHECK(Base64UrlDecode("-_8") == std::vector<uint8>{ 0xFB, 0xFF });
    CHECK_FALSE(Base64UrlDecode("-_9").has_value());     // non-zero padding bits
    CHECK_FALSE(Base64UrlDecode("A").has_value());

    std::string token = GenerateToken();
    CHECK(token.size() == 43);
    CHECK(token != GenerateToken());
    CHECK(Base64UrlDecode(token)->size() == 32);

    CHECK(Sha256Hex("abc") == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    CHECK(ConstantTimeEquals("abc", "abc"));
    CHECK_FALSE(ConstantTimeEquals("abc", "abd"));
    CHECK_FALSE(ConstantTimeEquals("abc", "ab"));

    std::vector<std::string> allowed{ "https://blog.example.com" };
    Rooftree::Net::Http::Request request;
    CHECK_FALSE(CheckOrigin(request, allowed));
    request.set(boost::beast::http::field::referer, "https://blog.example.com/zh/posts/x?y");
    CHECK(CheckOrigin(request, allowed));
    request.set(boost::beast::http::field::origin, "https://evil.example.com");
    CHECK_FALSE(CheckOrigin(request, allowed));
    request.set(boost::beast::http::field::origin, "HTTPS://Blog.Example.com");
    CHECK(CheckOrigin(request, allowed));
    request.set(boost::beast::http::field::origin, "null");
    CHECK_FALSE(CheckOrigin(request, allowed));

    CHECK_FALSE(CheckCsrfToken(request, "token"));
    request.set("X-CSRF-Token", "token");
    CHECK(CheckCsrfToken(request, "token"));
    CHECK_FALSE(CheckCsrfToken(request, ""));

    CHECK(IsSafeMethod(boost::beast::http::verb::get));
    CHECK_FALSE(IsSafeMethod(boost::beast::http::verb::post));

    CHECK(BuildTotpUri("My Blog", "admin", bytes)
        == "otpauth://totp/My%20Blog:admin?secret=MZXW6YTBOI&issuer=My%20Blog&algorithm=SHA1&digits=6&period=30");
}

TEST_CASE("Mailer builds MIME messages", "[server][mail]")
{
    MailSettings settings{ .From = "noreply@example.com", .FromName = "博客" };
    MailMessage message{ .To = "user@example.com", .Subject = "验证邮箱", .TextBody = "hello", .HtmlBody = "<p>hello</p>" };

    std::string mail = Mailer::BuildMessage(settings, message, 0, "id@example.com");
    CHECK(mail.starts_with("Date: Thu, 01 Jan 1970 00:00:00 +0000\r\n"));
    CHECK(mail.find("From: =?UTF-8?B?5Y2a5a6i?= <noreply@example.com>\r\n") != std::string::npos);
    CHECK(mail.find("Subject: =?UTF-8?B?6aqM6K+B6YKu566x?=\r\n") != std::string::npos);
    CHECK(mail.find("Content-Type: multipart/alternative;") != std::string::npos);
    CHECK(mail.find("aGVsbG8=\r\n") != std::string::npos);                 // "hello"
    CHECK(mail.find("PHA+aGVsbG88L3A+\r\n") != std::string::npos);         // "<p>hello</p>"

    MailMessage injected = message;
    injected.Subject = "hi\r\nBcc: victim@example.com";
    CHECK(Mailer::BuildMessage(settings, injected, 0, "id").empty());

    MailMessage plain{ .To = "user@example.com", .Subject = "Plain", .TextBody = "x" };
    std::string plainMail = Mailer::BuildMessage(settings, plain, 0, "id");
    CHECK(plainMail.find("Subject: Plain\r\n") != std::string::npos);
    CHECK(plainMail.find("Content-Type: text/plain; charset=UTF-8\r\n") != std::string::npos);

    MailMessage list = plain;
    list.Headers = { { "List-Unsubscribe", "<https://example.com/api/unsubscribe?token=x>" }, { "List-Unsubscribe-Post", "List-Unsubscribe=One-Click" } };
    std::string listMail = Mailer::BuildMessage(settings, list, 0, "id");
    CHECK(listMail.find("List-Unsubscribe: <https://example.com/api/unsubscribe?token=x>\r\nList-Unsubscribe-Post: List-Unsubscribe=One-Click\r\n")
        != std::string::npos);

    // Headers can't smuggle others in
    list.Headers = { { "X-Note", "a\r\nBcc: victim@example.com" } };
    CHECK(Mailer::BuildMessage(settings, list, 0, "id").empty());
    list.Headers = { { "Bcc: victim@example.com\r\nX", "a" } };
    CHECK(Mailer::BuildMessage(settings, list, 0, "id").empty());
}
