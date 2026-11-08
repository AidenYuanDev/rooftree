/*
 * rooftree-server's session core: what one message costs on its way through SessionMgr (inbox, session
 * lookup, AntiDOS, Process) with handlers that do nothing. The rest of an update's time belongs to the
 * handlers, so MaxMessagesPerUpdate = (the update time a server may spend on messages) / (this cost +
 * a typical handler's). docs/29-server-core.md has the numbers of one machine.
 */

#include "rooftree_benchmark.h"

#include "SessionMgr.h"
#include "Task.h"
#include <string>
#include <vector>

using namespace Rooftree::Server;

namespace
{
class NopMessage final : public Message
{
public:
    using Message::Message;

    void Process(Session&) override { }
    void Reject(RejectReason) override { }
};

class NopSessionMgr final : public SessionMgr
{
public:
    NopSessionMgr() : SessionMgr(1) { }

protected:
    std::unique_ptr<Session> CreateSession(Message const& firstMessage) override
    {
        return std::make_unique<Session>(*this, firstMessage.GetSessionKey(), firstMessage.GetRemoteAddress());
    }
};

// messages messages spread over sessions sessions, enqueued and run in one update
uint32 RunUpdate(NopSessionMgr& mgr, MessageType const& type, std::vector<std::string> const& keys, uint32 messages)
{
    boost::asio::ip::address address = boost::asio::ip::make_address("127.0.0.1");
    for (uint32 i = 0; i < messages; ++i)
        mgr.Enqueue(std::make_unique<NopMessage>(type, keys[i % keys.size()], address));

    mgr.Update(1);
    return uint32(mgr.GetStats().ProcessedMessages);
}

std::vector<std::string> MakeKeys(std::size_t count)
{
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < count; ++i)
        keys.push_back("ip:10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256));
    return keys;
}
}

namespace
{
Rooftree::Task<void> NopOnWorker(Session& session, int& left)
{
    co_await session.OnWorker([] { });
    --left;
}
}

// What the worker round trip costs, the worker and the main thread's loop included
TEST_CASE("A worker round trip, callback or coroutine", "[server]")
{
    NopSessionMgr mgr;
    MessageType const& type = mgr.GetMessageTypes().Register("nop");
    mgr.LoadSettings({ .SessionIdleTimeout = 1h }, {});
    mgr.Enqueue(std::make_unique<NopMessage>(type, "a", boost::asio::ip::make_address("10.0.0.1")));
    mgr.Update(1);
    Session& session = *mgr.FindSession("a");

    int constexpr Rounds = 1000;
    auto run = [&](auto start)
    {
        int left = Rounds;
        for (int i = 0; i < Rounds; ++i)
            start(left);
        while (left > 0)
            mgr.Update(1);
        return left;
    };

    BENCHMARK("co_await OnWorker: 1000 round trips")
    {
        return run([&](int& left) { session.Spawn(NopOnWorker(session, left)); });
    };
}

TEST_CASE("SessionMgr", "[server]")
{
    for (std::size_t sessions : { 1, 100, 1000 })
    {
        NopSessionMgr mgr;
        MessageType const& type = mgr.GetMessageTypes().Register("nop");
        MessageType const& limited = mgr.GetMessageTypes().Register("limited");
        AntiDosSettings antiDos;
        antiDos.Limits["limited"] = { .Window = 1h, .MaxCount = 1000000 };
        // Every message of an update runs in that update
        mgr.LoadSettings({ .MaxMessagesPerSessionPerUpdate = 1000, .MaxQueuedMessagesPerSession = 1000, .SessionIdleTimeout = 1h }, antiDos);
        std::vector<std::string> keys = MakeKeys(sessions);
        RunUpdate(mgr, type, keys, uint32(sessions));   // creates the sessions

        BENCHMARK(std::to_string(sessions) + " sessions: 1000 messages in one update")
        {
            return RunUpdate(mgr, type, keys, 1000);
        };

        BENCHMARK(std::to_string(sessions) + " sessions: 1000 AntiDOS limited messages in one update")
        {
            return RunUpdate(mgr, limited, keys, 1000);
        };

        BENCHMARK(std::to_string(sessions) + " sessions: an update without messages")
        {
            mgr.Update(1);
            return mgr.GetSessionCount();
        };
    }
}
