/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * A client that cheats and sends garbage, to see the server refuse it. It logs in as <prefix>_r (the
 * rogue), <prefix>_v (its victim, which stands still) and <prefix>_x (thrown away by the tests that
 * get the connection closed), then runs each check against what the server answers: bad packets,
 * every cast the rules forbid, floods, and logging in again to get out of trouble.
 *
 * Usage: arena_rogue [host] [http port] [prefix]
 * Run it against a server nobody else plays on: other players would walk in and fight. Prints one
 * line per check, PASS or FAIL with what was seen; the exit code is the number of failures.
 */

#include "Arena_generated.h"
#include "IoContext.h"
#include "KcpClient.h"
#include "Util.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <format>
#include <print>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace std::chrono_literals;
namespace http = boost::beast::http;
namespace Kcp = Rooftree::Net::Kcp;

namespace
{
constexpr std::size_t PacketHeaderSize = 8;
constexpr char const* Password = "rogue-password";

// Not in KcpProtocol.h: the arena closes with the WebSocket codes
constexpr uint16 InvalidPayload = 1007;

enum : uint32
{
    SPELL_STRIKE   = 1,
    SPELL_FIREBALL = 2,
    SPELL_POISON   = 3,
    SPELL_HEAL     = 4,
    SPELL_HAMMER   = 5,
};

using Clock = std::chrono::steady_clock;

std::string MakePacket(Arena::Opcode opcode, std::string_view payload)
{
    std::string packet(PacketHeaderSize, '\0');
    packet[0] = char(uint16(opcode) & 0xFF);
    packet[1] = char(uint16(opcode) >> 8);
    packet.append(payload);
    return packet;
}

std::string MakePacket(Arena::Opcode opcode, flatbuffers::FlatBufferBuilder const& builder)
{
    return MakePacket(opcode, std::string_view(reinterpret_cast<char const*>(builder.GetBufferPointer()), builder.GetSize()));
}

template <class Table>
Table const* Read(std::vector<uint8> const& payload)
{
    flatbuffers::Verifier verifier(payload.data(), payload.size());
    return verifier.VerifyBuffer<Table>(nullptr) ? flatbuffers::GetRoot<Table>(payload.data()) : nullptr;
}

// One connection and everything the server told it. Its members are read under Lock, Wait() holds it
// while the predicate looks
class Probe final : public Kcp::Client
{
public:
    struct Seen
    {
        float X;
        float Y;
        uint32 Health;
        uint32 MaxHealth;
        uint32 Mana;
        uint32 MaxMana;
        uint32 Target;
    };

    struct Event
    {
        Arena::CombatEventType Type;
        uint32 Caster;
        uint32 Target;
        uint32 Spell;
        uint32 Amount;
        Arena::InterruptReason Reason;
    };

    using Client::Client;

    template <class Predicate>
    bool Wait(Predicate predicate, Clock::duration timeout)
    {
        std::unique_lock lock(Lock);
        return Changed.wait_for(lock, timeout, [&] { return predicate(); });
    }

    // Under Lock
    Seen const* Find(uint32 id) const
    {
        auto itr = Players.find(id);
        return itr != Players.end() ? &itr->second : nullptr;
    }

    // Unlocked copies, for the checks
    Optional<Seen> Get(uint32 id)
    {
        std::lock_guard lock(Lock);
        Seen const* seen = Find(id);
        return seen ? Optional<Seen>(*seen) : Optional<Seen>();
    }

    std::size_t EventCount()
    {
        std::lock_guard lock(Lock);
        return Events.size();
    }

    std::vector<Event> EventsSince(std::size_t start)
    {
        std::lock_guard lock(Lock);
        return { Events.begin() + std::min(start, Events.size()), Events.end() };
    }

    // The next State after now
    bool WaitState(Clock::duration timeout = 1s)
    {
        uint64 states;
        {
            std::lock_guard lock(Lock);
            states = States;
        }
        return Wait([&] { return States > states; }, timeout);
    }

    std::function<void(Probe&)> OnOpenHook;

    std::mutex Lock;
    std::condition_variable Changed;
    uint32 Id = 0;
    float Radius = 16.0f;
    float Width = 0.0f;
    float Height = 0.0f;
    std::unordered_map<uint32, Seen> Players;
    uint64 States = 0;
    std::vector<std::pair<uint32, Arena::CastError>> Failed;     // castId, error
    std::vector<Event> Events;
    std::vector<std::string> Chats;
    std::vector<std::string> Systems;
    uint32 Pongs = 0;
    bool Opened = false;
    bool Closed = false;
    uint16 CloseCode = 0;
    std::string CloseReason;

protected:
    void OnOpen() override
    {
        if (OnOpenHook)
            OnOpenHook(*this);
        std::lock_guard lock(Lock);
        Opened = true;
        Changed.notify_all();
    }

    void OnMessage(std::string&& packet, bool /*reliable*/) override
    {
        if (packet.size() < PacketHeaderSize)
            return;

        Arena::Opcode opcode = Arena::Opcode(uint8(packet[0]) | uint8(packet[1]) << 8);
        std::vector<uint8> payload(packet.begin() + PacketHeaderSize, packet.end());

        std::lock_guard lock(Lock);
        switch (opcode)
        {
            case Arena::Opcode::Welcome:
                if (Arena::Welcome const* welcome = Read<Arena::Welcome>(payload))
                {
                    Id = welcome->id();
                    Radius = welcome->playerRadius();
                    Width = welcome->width();
                    Height = welcome->height();
                }
                break;
            case Arena::Opcode::State:
                if (Arena::State const* state = Read<Arena::State>(payload); state && state->players())
                {
                    Players.clear();
                    for (Arena::PlayerState const* player : *state->players())
                        Players[player->id()] = { player->x(), player->y(), player->health(), player->maxHealth(), player->mana(), player->maxMana(), player->target() };
                    ++States;
                }
                break;
            case Arena::Opcode::CastFailed:
                if (Arena::CastFailed const* failed = Read<Arena::CastFailed>(payload))
                    Failed.emplace_back(failed->castId(), failed->error());
                break;
            case Arena::Opcode::CombatLog:
                if (Arena::CombatLog const* log = Read<Arena::CombatLog>(payload); log && log->events())
                    for (Arena::CombatEvent const* event : *log->events())
                        Events.push_back({ event->type(), event->caster(), event->target(), event->spell(), event->amount(), event->reason() });
                break;
            case Arena::Opcode::ChatMessage:
                if (Arena::ChatMessage const* chat = Read<Arena::ChatMessage>(payload); chat && chat->text())
                    Chats.push_back(chat->text()->str());
                break;
            case Arena::Opcode::SystemMessage:
                if (Arena::SystemMessage const* system = Read<Arena::SystemMessage>(payload); system && system->text())
                    Systems.push_back(system->text()->str());
                break;
            case Arena::Opcode::Pong:
                ++Pongs;
                break;
            default:
                break;
        }
        Changed.notify_all();
    }

    void OnClose(uint16 code, std::string_view reason) override
    {
        std::lock_guard lock(Lock);
        Closed = true;
        CloseCode = code;
        CloseReason = reason;
        Changed.notify_all();
    }
};

std::atomic<uint32> NextCastId = 0;

void SendInput(Probe& probe, int8 dx, int8 dy)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Arena::CreateInput(builder, dx, dy));
    probe.Send(MakePacket(Arena::Opcode::Input, builder));
}

uint32 SendCast(Probe& probe, uint32 spell, uint32 target)
{
    uint32 castId = ++NextCastId;
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Arena::CreateCastSpell(builder, spell, target, castId));
    probe.Send(MakePacket(Arena::Opcode::CastSpell, builder));
    return castId;
}

void SendSetTarget(Probe& probe, uint32 target)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Arena::CreateSetTarget(builder, target));
    probe.Send(MakePacket(Arena::Opcode::SetTarget, builder));
}

void SendCancel(Probe& probe)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Arena::CreateCancelCast(builder));
    probe.Send(MakePacket(Arena::Opcode::CancelCast, builder));
}

void SendChat(Probe& probe, std::string_view bytes)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Arena::CreateChat(builder, builder.CreateString(bytes.data(), bytes.size())));
    probe.Send(MakePacket(Arena::Opcode::Chat, builder));
}

void SendPing(Probe& probe)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Arena::CreatePing(builder, 1.0));
    probe.Send(MakePacket(Arena::Opcode::Ping, builder));
}

// The cast's refusal, if one comes within the timeout
Optional<Arena::CastError> WaitFailed(Probe& probe, uint32 castId, Clock::duration timeout = 500ms)
{
    Optional<Arena::CastError> error;
    probe.Wait([&]
    {
        for (auto& [id, failed] : probe.Failed)
            if (id == castId)
                error = failed;
        return error.has_value();
    }, timeout);
    return error;
}

char const* ErrorName(Optional<Arena::CastError> error)
{
    return error ? Arena::EnumNameCastError(*error) : "accepted";
}

// HTTP
std::pair<http::status, boost::json::value> Post(std::string const& host, std::string const& port, std::string_view target, boost::json::object const& body)
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::resolver resolver(io);
    boost::asio::ip::tcp::socket socket(io);
    boost::asio::connect(socket, resolver.resolve(host, port));

    http::request<http::string_body> request(http::verb::post, target, 11);
    request.set(http::field::host, host);
    request.set(http::field::content_type, "application/json");
    request.body() = boost::json::serialize(body);
    request.prepare_payload();
    http::write(socket, request);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    boost::system::error_code error;
    boost::json::value json = boost::json::parse(response.body(), error);
    return { response.result(), json };
}

struct Login
{
    std::string Ticket;
    Kcp::Key Key;
    uint16 KcpPort;
};

struct Rogue
{
    std::string Host;
    std::string HttpPort;
    std::string Prefix;
    Rooftree::Asio::IoContext Context;
    uint32 Passed = 0;
    uint32 FailedChecks = 0;

    void Check(std::string_view name, bool ok, std::string const& detail)
    {
        std::print("{} {:<44} {}\n", ok ? "PASS" : "FAIL", name, detail);
        std::fflush(stdout);
        ++(ok ? Passed : FailedChecks);
    }

    Optional<Login> LogIn(std::string const& name)
    {
        boost::json::object credentials{ { "username", name }, { "password", Password } };
        auto [registered, registerAnswer] = Post(Host, HttpPort, "/api/register", credentials);
        if (registered != http::status::ok && registered != http::status::conflict)
            return {};

        auto [status, answer] = Post(Host, HttpPort, "/api/login", credentials);
        boost::json::object const* login = answer.if_object();
        if (status != http::status::ok || !login || !login->contains("kcpPort"))
            return {};

        Login result;
        result.Ticket = std::string(login->at("ticket").as_string());
        result.KcpPort = uint16(login->at("kcpPort").as_int64());
        std::vector<uint8> key = Rooftree::FromHex(login->at("kcpKey").as_string()).value_or(std::vector<uint8>());
        if (key.size() != result.Key.size())
            return {};
        std::copy(key.begin(), key.end(), result.Key.begin());
        return result;
    }

    std::shared_ptr<Probe> Connect(Login const& login, std::function<void(Probe&)> onOpen = {})
    {
        boost::asio::ip::udp::resolver resolver(Context);
        boost::asio::ip::udp::endpoint server = *resolver.resolve(boost::asio::ip::udp::v4(), Host, std::to_string(login.KcpPort)).begin();
        auto probe = std::make_shared<Probe>(Context, Kcp::Settings());
        probe->OnOpenHook = std::move(onOpen);
        probe->Connect(server, login.Ticket, login.Key);
        return probe;
    }

    // Logged in, the Welcome and a State with itself in it
    std::shared_ptr<Probe> Join(std::string const& name)
    {
        Optional<Login> login = LogIn(name);
        if (!login)
            return nullptr;
        std::shared_ptr<Probe> probe = Connect(*login);
        if (!probe->Wait([&] { return probe->Id && probe->Find(probe->Id); }, 5s))
            return nullptr;
        return probe;
    }

    // Walks up to the other one until their edges are this close, false if it didn't get there
    static bool WalkTo(Probe& probe, uint32 targetId, float edgeDistance, Clock::duration timeout = 8s)
    {
        Clock::time_point end = Clock::now() + timeout;
        int8 lastX = 0, lastY = 0;
        while (Clock::now() < end)
        {
            probe.WaitState();
            Optional<Probe::Seen> me = probe.Get(probe.Id);
            Optional<Probe::Seen> target = probe.Get(targetId);
            if (!me || !target)
                return false;

            float dx = target->X - me->X;
            float dy = target->Y - me->Y;
            if (std::hypot(dx, dy) - 2 * probe.Radius <= edgeDistance)
            {
                SendInput(probe, 0, 0);
                probe.WaitState();
                probe.WaitState();
                return true;
            }

            int8 inputX = std::abs(dx) > 3.0f ? (dx > 0 ? 1 : -1) : 0;
            int8 inputY = std::abs(dy) > 3.0f ? (dy > 0 ? 1 : -1) : 0;
            if (inputX != lastX || inputY != lastY)
                SendInput(probe, inputX, inputY);
            lastX = inputX;
            lastY = inputY;
        }
        SendInput(probe, 0, 0);
        return false;
    }

    static float EdgeDistance(Probe& probe, uint32 a, uint32 b)
    {
        Optional<Probe::Seen> first = probe.Get(a);
        Optional<Probe::Seen> second = probe.Get(b);
        if (!first || !second)
            return -1.0f;
        return std::max(0.0f, std::hypot(first->X - second->X, first->Y - second->Y) - 2 * probe.Radius);
    }

    // A connection of its own, <prefix>_x, for a check that gets it closed: the close code and
    // reason, or nothing if it stayed open for the timeout
    struct CloseResult
    {
        bool Closed;
        uint16 Code;
        std::string Reason;
        uint32 Pongs;
    };

    CloseResult SendAndWatch(std::function<void(Probe&)> const& send, Clock::duration timeout = 1500ms)
    {
        std::shared_ptr<Probe> probe = Join(Prefix + "_x");
        if (!probe)
            return { true, 0, "could not join", 0 };

        send(*probe);
        probe->Wait([&] { return probe->Closed; }, timeout);

        // Still there: does it still answer?
        if (!probe->Closed)
        {
            uint32 pongs;
            {
                std::lock_guard lock(probe->Lock);
                pongs = probe->Pongs;
            }
            SendPing(*probe);
            probe->Wait([&] { return probe->Pongs > pongs || probe->Closed; }, 1s);
        }

        std::lock_guard lock(probe->Lock);
        CloseResult result{ probe->Closed, probe->CloseCode, probe->CloseReason, probe->Pongs };
        if (!probe->Closed)
            probe->Close();
        return result;
    }

    static std::string Describe(CloseResult const& result)
    {
        if (!result.Closed)
            return std::format("stayed open, {} pongs", result.Pongs);
        return std::format("closed {} \"{}\"", result.Code, result.Reason);
    }

    void PacketChecks();
    void RuleChecks(Probe& rogue, Probe& victim);
    void SessionChecks(std::shared_ptr<Probe>& rogue, std::shared_ptr<Probe>& victim);
    void HttpChecks();
};

void Rogue::PacketChecks()
{
    CloseResult unknown = SendAndWatch([](Probe& probe) { probe.Send(MakePacket(Arena::Opcode(0x0099), "")); });
    Check("unknown opcode is ignored", !unknown.Closed && unknown.Pongs, Describe(unknown));

    CloseResult serverOpcode = SendAndWatch([](Probe& probe)
    {
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(Arena::CreateStateDirect(builder));
        probe.Send(MakePacket(Arena::Opcode::State, builder));
    });
    Check("a server opcode from the client is ignored", !serverOpcode.Closed && serverOpcode.Pongs, Describe(serverOpcode));

    CloseResult shortPacket = SendAndWatch([](Probe& probe) { probe.Send(std::string("\x05\x00\x00", 3)); });
    Check("packet shorter than its header closes", shortPacket.Closed && shortPacket.Code == InvalidPayload, Describe(shortPacket));

    CloseResult garbage = SendAndWatch([](Probe& probe)
    {
        probe.Send(MakePacket(Arena::Opcode::CastSpell, std::string_view("\xde\xad\xbe\xef\x01\x02\x03\x04\x05\x06\x07\x08", 12)));
    });
    Check("CastSpell of random bytes closes", garbage.Closed && garbage.Code == InvalidPayload, Describe(garbage));

    CloseResult offset = SendAndWatch([](Probe& probe)
    {
        // The root offset points 2 GB past the buffer
        probe.Send(MakePacket(Arena::Opcode::CastSpell, std::string_view("\xff\xff\xff\x7f\x00\x00\x00\x00", 8)));
    });
    Check("FlatBuffer offset out of bounds closes", offset.Closed && offset.Code == InvalidPayload, Describe(offset));

    CloseResult empty = SendAndWatch([](Probe& probe) { probe.Send(MakePacket(Arena::Opcode::CastSpell, "")); });
    Check("CastSpell with no table closes", empty.Closed && empty.Code == InvalidPayload, Describe(empty));

    CloseResult big = SendAndWatch([](Probe& probe) { SendChat(probe, std::string(5000, 'a')); });
    Check("5 KB message closes", big.Closed && big.Code == Kcp::CloseCode::MessageTooBig, Describe(big));

    CloseResult flood = SendAndWatch([](Probe& probe)
    {
        for (int i = 0; i < 1000; ++i)
            SendPing(probe);
    }, 3s);
    Check("1000 pings at once get the client kicked", flood.Closed && flood.Code == Kcp::CloseCode::PolicyViolation,
        std::format("{}, {} pongs before", Describe(flood), flood.Pongs));

    // Everything a player sends, before the server has loaded the player
    CloseResult early;
    {
        std::shared_ptr<Probe> probe;
        if (Optional<Login> login = LogIn(Prefix + "_x"))
            probe = Connect(*login, [](Probe& self)
            {
                SendInput(self, 1, 1);
                SendSetTarget(self, 1);
                SendCast(self, SPELL_STRIKE, 1);
                SendCast(self, SPELL_HEAL, 0);
                SendCancel(self);
                SendChat(self, "before the welcome");
            });
        bool welcomed = probe && probe->Wait([&] { return probe->Id || probe->Closed; }, 5s);
        early = { probe ? probe->Closed : true, probe ? probe->CloseCode : uint16(0), probe ? probe->CloseReason : "could not join", 0 };
        Check("messages before the player is loaded are ignored", welcomed && !early.Closed, welcomed ? Describe(early) : "no Welcome");
        if (probe && !probe->Closed)
            probe->Close();
    }
}

void Rogue::RuleChecks(Probe& rogue, Probe& victim)
{
    uint32 r = rogue.Id;
    uint32 v = victim.Id;

    for (uint32 spell : { 0u, 6u, 0xFFFFFFFFu })
    {
        Optional<Arena::CastError> error = WaitFailed(rogue, SendCast(rogue, spell, v));
        Check(std::format("spell id {} is UnknownSpell", spell), error == Arena::CastError::UnknownSpell, ErrorName(error));
    }

    Optional<Arena::CastError> self = WaitFailed(rogue, SendCast(rogue, SPELL_STRIKE, r));
    Check("Strike at yourself is BadTarget", self == Arena::CastError::BadTarget, ErrorName(self));

    Optional<Arena::CastError> none = WaitFailed(rogue, SendCast(rogue, SPELL_STRIKE, 0));
    Check("Strike at target 0 is NoTarget", none == Arena::CastError::NoTarget, ErrorName(none));

    Optional<Arena::CastError> missing = WaitFailed(rogue, SendCast(rogue, SPELL_STRIKE, 4000000000u));
    Check("Strike at a unit that isn't there is NoTarget", missing == Arena::CastError::NoTarget, ErrorName(missing));

    // Spawned anywhere on the field: almost never within 5 of each other
    float apart = EdgeDistance(rogue, r, v);
    if (apart > 10.0f)
    {
        Optional<Arena::CastError> far = WaitFailed(rogue, SendCast(rogue, SPELL_STRIKE, v));
        Check("Strike from afar is OutOfRange", far == Arena::CastError::OutOfRange, std::format("{} at {:.0f}", ErrorName(far), apart));
    }

    SendSetTarget(rogue, 4000000000u);
    rogue.WaitState();
    rogue.WaitState();
    Optional<Probe::Seen> seen = rogue.Get(r);
    Check("SetTarget at a unit that isn't there is no target", seen && seen->Target == 0, std::format("target {}", seen ? seen->Target : 0));

    // Walking: a cast with a cast time is refused, one started is broken off by a step
    SendInput(rogue, 1, 0);
    rogue.WaitState();
    rogue.WaitState();
    Optional<Arena::CastError> moving = WaitFailed(rogue, SendCast(rogue, SPELL_FIREBALL, v));
    Check("Fireball while walking is Moving", moving == Arena::CastError::Moving, ErrorName(moving));
    SendInput(rogue, 0, 0);
    rogue.WaitState();
    rogue.WaitState();

    // Fireball's range is 300
    if (!WalkTo(rogue, v, 200.0f))
        return Check("walk into Fireball range", false, "did not get there");
    std::this_thread::sleep_for(1100ms);   // the earlier casts' global cooldown

    std::size_t start = rogue.EventCount();
    uint32 fireball = SendCast(rogue, SPELL_FIREBALL, v);
    bool started = rogue.Wait([&] { return std::ranges::any_of(rogue.Events, [&](Probe::Event const& e) { return e.Type == Arena::CombatEventType::CastStart && e.Caster == r; }); }, 1s);
    SendInput(rogue, -1, 0);
    rogue.Wait([&] { return std::ranges::any_of(rogue.Events, [&](Probe::Event const& e) { return e.Type == Arena::CombatEventType::CastInterrupted && e.Caster == r; }); }, 1s);
    SendInput(rogue, 0, 0);
    std::this_thread::sleep_for(1700ms);     // past when it would have landed
    std::vector<Probe::Event> events = rogue.EventsSince(start);
    bool interrupted = std::ranges::any_of(events, [&](Probe::Event const& e) { return e.Type == Arena::CombatEventType::CastInterrupted && e.Caster == r && e.Reason == Arena::InterruptReason::Moved; });
    bool landed = std::ranges::any_of(events, [&](Probe::Event const& e) { return e.Type == Arena::CombatEventType::Damage && e.Caster == r && e.Spell == SPELL_FIREBALL; });
    Check("a step breaks off Fireball, no damage", started && interrupted && !landed,
        std::format("started {}, interrupted {}, landed {}, refused {}", started, interrupted, landed, ErrorName(WaitFailed(rogue, fireball, 0ms))));

    // Heal is on yourself whatever the packet says
    start = rogue.EventCount();
    Optional<Arena::CastError> heal = WaitFailed(rogue, SendCast(rogue, SPELL_HEAL, v), 300ms);
    rogue.Wait([&] { return std::ranges::any_of(rogue.Events, [&](Probe::Event const& e) { return e.Type == Arena::CombatEventType::SpellGo && e.Caster == r && e.Spell == SPELL_HEAL; }); }, 3s);
    events = rogue.EventsSince(start);
    bool healedVictim = std::ranges::any_of(events, [&](Probe::Event const& e) { return e.Caster == r && e.Spell == SPELL_HEAL && e.Target == v; });
    bool healedSelf = std::ranges::any_of(events, [&](Probe::Event const& e) { return e.Type == Arena::CombatEventType::SpellGo && e.Spell == SPELL_HEAL && e.Target == r; });
    Check("Heal at the enemy heals yourself", !heal && healedSelf && !healedVictim, std::format("{}, on self {}, on victim {}", ErrorName(heal), healedSelf, healedVictim));

    // Melee
    if (!WalkTo(rogue, v, 2.0f))
        return Check("walk into melee range", false, "did not get there");
    std::this_thread::sleep_for(1100ms);

    // 20 Strikes in one datagram burst: one tick runs them all, the global cooldown lets one through
    start = rogue.EventCount();
    std::vector<uint32> strikes;
    for (int i = 0; i < 20; ++i)
        strikes.push_back(SendCast(rogue, SPELL_STRIKE, v));
    std::this_thread::sleep_for(600ms);
    events = rogue.EventsSince(start);
    std::size_t hits = std::ranges::count_if(events, [&](Probe::Event const& e) { return e.Type == Arena::CombatEventType::SpellGo && e.Caster == r && e.Spell == SPELL_STRIKE; });
    std::size_t notReady = 0;
    for (uint32 castId : strikes)
        notReady += WaitFailed(rogue, castId, 0ms) == Arena::CastError::NotReady;
    Check("20 Strikes at once: 1 lands, 19 NotReady", hits == 1 && notReady == 19, std::format("{} landed, {} NotReady", hits, notReady));

    // Hammer: the stun holds the victim and its casts, its own cooldown refuses the next
    std::this_thread::sleep_for(1100ms);
    Optional<Probe::Seen> before = victim.Get(v);
    Optional<Arena::CastError> hammer = WaitFailed(rogue, SendCast(rogue, SPELL_HAMMER, v), 200ms);
    victim.Wait([&] { return std::ranges::any_of(victim.Events, [&](Probe::Event const& e) { return e.Type == Arena::CombatEventType::AuraApplied && e.Target == v; }); }, 1s);
    SendInput(victim, 1, 1);
    Optional<Arena::CastError> stunnedCast = WaitFailed(victim, SendCast(victim, SPELL_STRIKE, r));
    victim.WaitState();
    victim.WaitState();
    Optional<Probe::Seen> during = victim.Get(v);
    bool held = before && during && std::hypot(during->X - before->X, during->Y - before->Y) < 1.0f;
    Check("Hammer lands", !hammer, ErrorName(hammer));
    Check("a stunned victim can't walk", held, before && during ? std::format("moved {:.1f}", std::hypot(during->X - before->X, during->Y - before->Y)) : "no state");
    Check("a stunned victim can't cast", stunnedCast == Arena::CastError::Stunned, ErrorName(stunnedCast));
    SendInput(victim, 0, 0);

    std::this_thread::sleep_for(1100ms);
    Optional<Arena::CastError> again = WaitFailed(rogue, SendCast(rogue, SPELL_HAMMER, v));
    Check("Hammer again within 15 s is NotReady", again == Arena::CastError::NotReady, ErrorName(again));

    // The victim hits back a little: the rogue has something to lose for the relog checks
    std::this_thread::sleep_for(1500ms);
    for (int i = 0; i < 3; ++i)
    {
        SendCast(victim, SPELL_STRIKE, r);
        std::this_thread::sleep_for(1050ms);
    }

    // Poison until the mana runs out: refused before it would go below 0
    Optional<Arena::CastError> poison;
    uint32 poisons = 0;
    while (poisons < 12)
    {
        poison = WaitFailed(rogue, SendCast(rogue, SPELL_POISON, v), 300ms);
        if (poison == Arena::CastError::NoMana)
            break;
        if (!poison)
            ++poisons;
        std::this_thread::sleep_for(750ms);
    }
    rogue.WaitState();
    Optional<Probe::Seen> drained = rogue.Get(r);
    Check("Poison until NoMana, mana never below 0", poison == Arena::CastError::NoMana && drained && drained->Mana < 15 && drained->Mana <= drained->MaxMana,
        std::format("{} after {} casts, mana {}", ErrorName(poison), poisons, drained ? drained->Mana : 0));

    // Strike until the victim dies
    bool dead = false;
    for (int i = 0; i < 40 && !dead; ++i)
    {
        SendCast(rogue, SPELL_STRIKE, v);
        dead = rogue.Wait([&] { Probe::Seen const* s = rogue.Find(v); return s && !s->Health; }, 1050ms);
    }
    Check("the victim dies", dead, dead ? "" : "still alive after 40 Strikes");
    if (!dead)
        return;

    std::this_thread::sleep_for(1050ms);
    Optional<Arena::CastError> atCorpse = WaitFailed(rogue, SendCast(rogue, SPELL_STRIKE, v));
    Check("Strike at a dead victim is TargetDead", atCorpse == Arena::CastError::TargetDead, ErrorName(atCorpse));

    Optional<Arena::CastError> fromCorpse = WaitFailed(victim, SendCast(victim, SPELL_STRIKE, r));
    Check("a dead victim can't cast", fromCorpse == Arena::CastError::Dead, ErrorName(fromCorpse));

    before = victim.Get(v);
    SendInput(victim, -1, -1);
    victim.WaitState();
    victim.WaitState();
    victim.WaitState();
    during = victim.Get(v);
    SendInput(victim, 0, 0);
    Check("a dead victim can't walk", before && during && std::hypot(during->X - before->X, during->Y - before->Y) < 1.0f,
        before && during ? std::format("moved {:.1f}", std::hypot(during->X - before->X, during->Y - before->Y)) : "no state");
}

void Rogue::SessionChecks(std::shared_ptr<Probe>& rogue, std::shared_ptr<Probe>& victim)
{
    // Logging out and in again: a dead victim back before its respawn, a hurt rogue healed and its
    // cooldowns over. The server loads a player afresh when the old one has left
    Optional<Probe::Seen> hurt = rogue->Get(rogue->Id);
    Optional<Probe::Seen> corpse = victim->Get(victim->Id);
    rogue->Close();
    victim->Close();
    rogue->Wait([&] { return rogue->Closed; }, 2s);
    victim->Wait([&] { return victim->Closed; }, 2s);
    std::this_thread::sleep_for(200ms);  // the world thread takes them out

    victim = Join(Prefix + "_v");
    Optional<Probe::Seen> revived = victim ? victim->Get(victim->Id) : Optional<Probe::Seen>();
    Check("a dead player logging in again is still dead", corpse && !corpse->Health && revived && !revived->Health,
        revived ? std::format("health {} -> {} at once", corpse ? corpse->Health : 0, revived->Health) : "could not join");

    rogue = Join(Prefix + "_r");
    Optional<Probe::Seen> healed = rogue ? rogue->Get(rogue->Id) : Optional<Probe::Seen>();
    Check("a hurt player logging in again is still hurt", hurt && healed && hurt->Health < hurt->MaxHealth && healed->Health == hurt->Health,
        hurt && healed ? std::format("health {} -> {}, mana {} -> {}", hurt->Health, healed->Health, hurt->Mana, healed->Mana) : "no state");

    if (rogue && victim && WalkTo(*rogue, victim->Id, 2.0f))
    {
        // The rogue's Hammer was cast less than a minute ago, its cooldown is 15 s: wait out the
        // global cooldown only. A relog that keeps cooldowns refuses it if under 15 s have passed
        Optional<Arena::CastError> hammer = WaitFailed(*rogue, SendCast(*rogue, SPELL_HAMMER, victim->Id));
        std::print("     (Hammer right after the relog: {}; the last one was cast about 30 s ago, so this says little)\n", ErrorName(hammer));
    }

    // A ticket is good for one connection
    if (Optional<Login> login = LogIn(Prefix + "_x"))
    {
        std::shared_ptr<Probe> first = Connect(*login);
        bool firstIn = first->Wait([&] { return first->Id != 0; }, 5s);
        std::shared_ptr<Probe> second = Connect(*login);
        bool secondIn = second->Wait([&] { return second->Opened || second->Closed; }, 6s) && second->Opened;
        Check("a ticket used twice opens one connection", firstIn && !secondIn,
            std::format("first {}, second {} ({} {})", firstIn, secondIn, second->CloseCode, second->CloseReason));
        first->Close();
        second->Close();
        std::this_thread::sleep_for(200ms);
    }

    // Two logins at once: the second takes over the player, the first is kicked
    {
        std::shared_ptr<Probe> first = Join(Prefix + "_x");
        std::shared_ptr<Probe> second = first ? Join(Prefix + "_x") : nullptr;
        bool kicked = first && first->Wait([&] { return first->Closed; }, 2s);
        Check("logging in again elsewhere kicks the first", kicked && second,
            first ? std::format("first closed {} \"{}\"", first->CloseCode, first->CloseReason) : "could not join");
        if (second)
            second->Close();
    }

    // Chat: the victim is the one who reads it
    if (rogue && victim)
    {
        auto chats = [&] { std::lock_guard lock(victim->Lock); return victim->Chats; };
        std::this_thread::sleep_for(600ms);
        std::size_t start = chats().size();
        SendChat(*rogue, std::string_view("\xff\xfe\xfd", 3));
        std::this_thread::sleep_for(600ms);
        Check("chat that isn't UTF-8 is dropped", chats().size() == start, std::format("{} lines", chats().size() - start));

        SendChat(*rogue, std::string(3000, 'w'));
        victim->Wait([&] { return victim->Chats.size() > start; }, 1s);
        std::vector<std::string> lines = chats();
        Check("3000 character chat is cut to 200", lines.size() == start + 1 && lines.back().size() <= 200,
            lines.size() > start ? std::format("{} characters", lines.back().size()) : "nothing");

        std::this_thread::sleep_for(600ms);
        start = chats().size();
        for (int i = 0; i < 10; ++i)
            SendChat(*rogue, std::format("spam {}", i));
        std::this_thread::sleep_for(800ms);
        Check("10 chat lines at once: 1 goes out", chats().size() - start == 1, std::format("{} went out", chats().size() - start));
    }

    // Input past -1..1 walks at the normal speed: 220 a second, diagonally too
    if (rogue)
    {
        Optional<Probe::Seen> from = rogue->Get(rogue->Id);
        if (from)
        {
            int8 dx = from->X < rogue->Width / 2 ? 127 : -128;
            int8 dy = from->Y < rogue->Height / 2 ? 127 : -128;
            SendInput(*rogue, dx, dy);
            Clock::time_point begin = Clock::now();
            std::this_thread::sleep_for(1000ms);
            Optional<Probe::Seen> to = rogue->Get(rogue->Id);
            double seconds = std::chrono::duration<double>(Clock::now() - begin).count();
            SendInput(*rogue, 0, 0);
            double speed = to ? std::hypot(to->X - from->X, to->Y - from->Y) / seconds : 0.0;
            Check("Input of 127/-128 walks at the normal speed", speed > 150.0 && speed < 260.0, std::format("{:.0f} a second", speed));
        }
    }
}

void Rogue::HttpChecks()
{
    // Guessing a password: is anything stopping it?
    boost::json::object wrong{ { "username", Prefix + "_r" }, { "password", "not-the-password" } };
    uint32 refused = 0, limited = 0, other = 0;
    Clock::time_point begin = Clock::now();
    for (int i = 0; i < 30; ++i)
    {
        http::status status = Post(Host, HttpPort, "/api/login", wrong).first;
        refused += status == http::status::unauthorized;
        limited += status == http::status::too_many_requests;
        other += status != http::status::unauthorized && status != http::status::too_many_requests;
    }
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count() / 30;
    Check("30 wrong passwords in a row get rate limited", limited > 0, std::format("{} refused, {} limited, {} other, {:.0f} ms each", refused, limited, other, ms));
}
}

int main(int argc, char** argv)
{
    Rogue rogue;
    rogue.Host = argc > 1 ? argv[1] : "127.0.0.1";
    rogue.HttpPort = argc > 2 ? argv[2] : "8090";
    rogue.Prefix = argc > 3 ? argv[3] : "rogue";

    auto work = boost::asio::make_work_guard(rogue.Context);
    std::thread thread([&] { rogue.Context.run(); });

    try
    {
        std::print("-- packets\n");
        rogue.PacketChecks();

        std::shared_ptr<Probe> attacker = rogue.Join(rogue.Prefix + "_r");
        std::shared_ptr<Probe> victim = rogue.Join(rogue.Prefix + "_v");
        if (!attacker || !victim || !attacker->Wait([&] { return attacker->Find(victim->Id) != nullptr; }, 3s))
            rogue.Check("rogue and victim join", false, "could not join");
        else
        {
            // A victim from an earlier run may still be dead
            victim->Wait([&] { Probe::Seen const* s = victim->Find(victim->Id); return s && s->Health; }, 6s);
            std::print("-- rules\n");
            rogue.RuleChecks(*attacker, *victim);
            std::print("-- sessions\n");
            rogue.SessionChecks(attacker, victim);
        }

        std::print("-- http\n");
        rogue.HttpChecks();

        if (attacker)
            attacker->Close();
        if (victim)
            victim->Close();
        std::this_thread::sleep_for(300ms);
    }
    catch (std::exception const& e)
    {
        std::print("error: {}\n", e.what());
        ++rogue.FailedChecks;
    }

    std::print("-- {} passed, {} failed\n", rogue.Passed, rogue.FailedChecks);
    work.reset();
    rogue.Context.stop();
    thread.join();
    return int(rogue.FailedChecks);
}
