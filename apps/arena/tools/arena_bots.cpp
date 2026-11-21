/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Bots that fight in the arena over KCP, to see the combat work and what it costs with many players.
 * Each registers (if it has to) and logs in as <prefix><n>, n of 4 digits at least, then picks the nearest living player and
 * fights it: even bots walk up and strike and stun, odd ones poison and cast fireballs from afar;
 * any of them heals when low. They play like a client, from what the server tells them and nothing
 * else, and every rule is the server's: a refused cast is counted, not prevented.
 *
 * Usage: arena_bots <count> [host] [http port] [seconds] [prefix]
 * Every 5 seconds and at the end: casts and refusals by reason, combat events, kills, and what each
 * bot got from the server per second.
 */

#include "Arena_generated.h"
#include "IoContext.h"
#include "KcpClient.h"
#include "Util.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <print>
#include <array>
#include <atomic>
#include <cmath>
#include <thread>
#include <unordered_map>

using namespace std::chrono_literals;
namespace http = boost::beast::http;
namespace Kcp = Rooftree::Net::Kcp;

namespace
{
constexpr std::size_t PacketHeaderSize = 8;
constexpr char const* Password = "bot-password";

enum : uint32
{
    SPELL_STRIKE   = 1,
    SPELL_FIREBALL = 2,
    SPELL_POISON   = 3,
    SPELL_HEAL     = 4,
    SPELL_HAMMER   = 5,
};

// What all the bots saw, added up
struct Stats
{
    std::atomic<uint64> CastsSent = 0;
    std::array<std::atomic<uint64>, std::size_t(Arena::CastError::MAX) + 1> CastFailed{};
    std::array<std::atomic<uint64>, std::size_t(Arena::CombatEventType::MAX) + 1> Events{};
    std::array<std::atomic<uint64>, std::size_t(Arena::InterruptReason::MAX) + 1> Interrupts{};
    std::atomic<uint64> Damage = 0;
    std::atomic<uint64> ReliableBytes = 0;
    std::atomic<uint64> UnreliableBytes = 0;
    std::atomic<uint64> Packets = 0;
    std::atomic<uint64> CombatLogs = 0;
    std::atomic<uint32> Open = 0;
    std::atomic<uint32> Closed = 0;
} stats;

std::string MakePacket(Arena::Opcode opcode, flatbuffers::FlatBufferBuilder const& builder)
{
    std::string packet(PacketHeaderSize, '\0');
    packet[0] = char(uint16(opcode) & 0xFF);
    packet[1] = char(uint16(opcode) >> 8);
    packet.append(reinterpret_cast<char const*>(builder.GetBufferPointer()), builder.GetSize());
    return packet;
}

template <class Table>
Table const* Read(std::vector<uint8> const& payload)
{
    flatbuffers::Verifier verifier(payload.data(), payload.size());
    return verifier.VerifyBuffer<Table>(nullptr) ? flatbuffers::GetRoot<Table>(payload.data()) : nullptr;
}

using Clock = std::chrono::steady_clock;

class Bot final : public Kcp::Client
{
public:
    Bot(Rooftree::Asio::IoContext& ioContext, Kcp::Settings const& settings, bool melee)
        : Client(ioContext, settings), _melee(melee) { }

protected:
    void OnOpen() override
    {
        ++stats.Open;
    }

    void OnMessage(std::string&& packet, bool reliable) override
    {
        ++stats.Packets;
        (reliable ? stats.ReliableBytes : stats.UnreliableBytes) += packet.size();
        if (packet.size() < PacketHeaderSize)
            return;

        Arena::Opcode opcode = Arena::Opcode(uint8(packet[0]) | uint8(packet[1]) << 8);
        std::vector<uint8> payload(packet.begin() + PacketHeaderSize, packet.end());
        switch (opcode)
        {
            case Arena::Opcode::Welcome:
                if (Arena::Welcome const* welcome = Read<Arena::Welcome>(payload))
                    _id = welcome->id();
                break;
            case Arena::Opcode::Units:
                if (Arena::Units const* units = Read<Arena::Units>(payload))
                {
                    if (units->units())
                        for (Arena::UnitInfo const* unit : *units->units())
                            _maxHealth[unit->id()] = unit->maxHealth();
                    if (units->gone())
                        for (uint32 id : *units->gone())
                            _maxHealth.erase(id);
                }
                break;
            case Arena::Opcode::Frame:
                if (Arena::Frame const* frame = Read<Arena::Frame>(payload); frame && frame->tick() > _lastTick)
                {
                    _lastTick = frame->tick();
                    OnFrame(*frame);
                }
                break;
            case Arena::Opcode::CombatLog:
                if (Arena::CombatLog const* log = Read<Arena::CombatLog>(payload))
                    OnCombatLog(*log);
                break;
            case Arena::Opcode::CastFailed:
                if (Arena::CastFailed const* failed = Read<Arena::CastFailed>(payload))
                {
                    ++stats.CastFailed[std::size_t(failed->error())];
                    _casting = false;
                }
                break;
            default:
                break;
        }
    }

    void OnClose(uint16 code, std::string_view reason) override
    {
        ++stats.Closed;
        if (code != Kcp::CloseCode::Normal)
            std::print(stderr, "bot {} closed: {} {}\n", _id, code, reason);
    }

private:
    struct Seen
    {
        float X;
        float Y;
        uint32 Health;
        uint32 MaxHealth;
    };

    void OnCombatLog(Arena::CombatLog const& log)
    {
        ++stats.CombatLogs;
        if (!log.events())
            return;

        for (Arena::CombatEvent const* event : *log.events())
        {
            // Every bot sees every event: counted once, by the bot it is about
            bool mine = event->caster() == _id;
            if (mine)
                ++stats.Events[std::size_t(event->type())];

            switch (event->type())
            {
                case Arena::CombatEventType::CastStart:
                    if (mine)
                        _casting = true;
                    break;
                case Arena::CombatEventType::SpellGo:
                    if (mine)
                        _casting = false;
                    break;
                case Arena::CombatEventType::CastInterrupted:
                    if (mine)
                    {
                        ++stats.Interrupts[std::size_t(event->reason())];
                        _casting = false;
                    }
                    break;
                case Arena::CombatEventType::Damage:
                    if (mine)
                        stats.Damage += event->amount();
                    break;
                case Arena::CombatEventType::Death:
                    if (event->target() == _id)
                        _casting = false;
                    break;
                default:
                    break;
            }
        }
    }

    void OnFrame(Arena::Frame const& frame)
    {
        if (!_id || !frame.units())
            return;

        _seen.clear();
        for (Arena::UnitMotion const* unit : *frame.units())
            _seen[unit->id()] = { unit->x(), unit->y(), unit->health(), _maxHealth[unit->id()] };

        auto me = _seen.find(_id);
        if (me == _seen.end() || !me->second.Health)
        {
            SetInput(0, 0);
            return;
        }

        // The nearest living one, kept while it lives: switching every tick would spread the damage
        auto target = _seen.find(_target);
        if (target == _seen.end() || !target->second.Health)
        {
            _target = 0;
            float best = 0.0f;
            for (auto& [id, seen] : _seen)
            {
                if (id == _id || !seen.Health)
                    continue;
                float distance = std::hypot(seen.X - me->second.X, seen.Y - me->second.Y);
                if (!_target || distance < best)
                {
                    _target = id;
                    best = distance;
                }
            }
            if (!_target)
            {
                SetInput(0, 0);
                return;
            }

            flatbuffers::FlatBufferBuilder builder;
            builder.Finish(Arena::CreateSetTarget(builder, _target));
            Send(MakePacket(Arena::Opcode::SetTarget, builder));
            // Melee also swings at it between its strikes
            if (_melee)
            {
                flatbuffers::FlatBufferBuilder attack;
                attack.Finish(Arena::CreateAttack(attack, _target));
                Send(MakePacket(Arena::Opcode::Attack, attack));
            }
            target = _seen.find(_target);
        }

        // Between centers: two radii (32) is touching. Melee walks up to it, a caster until in
        // fireball range and then stands its ground
        float dx = target->second.X - me->second.X;
        float dy = target->second.Y - me->second.Y;
        float distance = std::hypot(dx, dy);
        float wanted = _melee ? 34.0f : 280.0f;

        if (_casting)
            return;     // a step would interrupt it

        if (distance > wanted)
        {
            SetInput(int8(std::abs(dx) > 8.0f ? (dx > 0 ? 1 : -1) : 0), int8(std::abs(dy) > 8.0f ? (dy > 0 ? 1 : -1) : 0));
            if (_melee)
                return;
        }
        else
            SetInput(0, 0);

        // A cast a quarter of a second at most, the server says whether it can be
        Clock::time_point now = Clock::now();
        if (now < _nextCast)
            return;
        _nextCast = now + 250ms;

        if (me->second.Health * 5 < me->second.MaxHealth * 2 && !_moving)
            return Cast(SPELL_HEAL, _id);
        if (_melee)
            return Cast(now > _nextHammer ? SPELL_HAMMER : SPELL_STRIKE, _target);
        if (_moving)
            return Cast(SPELL_POISON, _target);
        Cast(now > _nextPoison ? SPELL_POISON : SPELL_FIREBALL, _target);
    }

    void Cast(uint32 spell, uint32 target)
    {
        // The bot's own guesses, the server keeps the real cooldowns
        if (spell == SPELL_HAMMER)
            _nextHammer = Clock::now() + 15s;
        if (spell == SPELL_POISON)
            _nextPoison = Clock::now() + 6s;

        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(Arena::CreateCastSpell(builder, spell, target, ++_castId));
        Send(MakePacket(Arena::Opcode::CastSpell, builder));
        ++stats.CastsSent;
    }

    void SetInput(int8 dx, int8 dy)
    {
        _moving = dx || dy;
        if (dx == _inputX && dy == _inputY)
            return;
        _inputX = dx;
        _inputY = dy;

        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(Arena::CreateInput(builder, dx, dy));
        Send(MakePacket(Arena::Opcode::Input, builder));
    }

    bool _melee;
    uint32 _id = 0;
    uint32 _target = 0;
    uint32 _castId = 0;
    bool _casting = false;
    bool _moving = false;
    int8 _inputX = 0;
    int8 _inputY = 0;
    Clock::time_point _nextCast;
    Clock::time_point _nextHammer;
    Clock::time_point _nextPoison;
    std::unordered_map<uint32, Seen> _seen;
    std::unordered_map<uint32, uint32> _maxHealth;      // Units'
    uint32 _lastTick = 0;
};

struct Login
{
    std::string Ticket;
    Kcp::Key Key;
    uint16 KcpPort;
};

// POST, the answer's status and JSON; throws on a network error
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
    return { response.result(), boost::json::parse(response.body()) };
}

Optional<Login> LogIn(std::string const& host, std::string const& port, std::string const& name)
{
    boost::json::object credentials{ { "username", name }, { "password", Password } };
    try
    {
        // Taken is fine: the bot's from an earlier run
        auto [registered, registerAnswer] = Post(host, port, "/api/register", credentials);
        if (registered != http::status::ok && registered != http::status::conflict)
        {
            std::print(stderr, "{}: register failed: {}\n", name, boost::json::serialize(registerAnswer));
            return {};
        }

        auto [status, answer] = Post(host, port, "/api/login", credentials);
        boost::json::object const* login = answer.if_object();
        if (status != http::status::ok || !login || !login->contains("kcpPort"))
        {
            std::print(stderr, "{}: login failed ({}), or the server has KCP off\n", name, boost::json::serialize(answer));
            return {};
        }

        Login result;
        result.Ticket = std::string(login->at("ticket").as_string());
        result.KcpPort = uint16(login->at("kcpPort").as_int64());
        std::vector<uint8> key = Rooftree::FromHex(login->at("kcpKey").as_string()).value_or(std::vector<uint8>());
        if (key.size() != result.Key.size())
            return {};
        std::copy(key.begin(), key.end(), result.Key.begin());
        return result;
    }
    catch (std::exception const& e)
    {
        std::print(stderr, "{}: {}\n", name, e.what());
        return {};
    }
}

void PrintStats(char const* title, double seconds, uint32 bots)
{
    static constexpr char const* CastErrors[] = { "Ok", "UnknownSpell", "Dead", "Casting", "Stunned", "Moving", "NotReady", "NoMana",
        "NoTarget", "TargetDead", "BadTarget", "OutOfRange", "NoComboPoints" };
    static constexpr char const* EventTypes[] = { "CastStart", "SpellGo", "CastInterrupted", "Damage", "Heal", "AuraApplied", "AuraRemoved",
        "Death", "Swing" };
    static constexpr char const* Reasons[] = { "None", "Cancelled", "Moved", "Stunned", "Died", "TargetLost", "OutOfRange" };
    static_assert(std::size(CastErrors) == std::tuple_size_v<decltype(stats.CastFailed)>);
    static_assert(std::size(EventTypes) == std::tuple_size_v<decltype(stats.Events)>);
    static_assert(std::size(Reasons) == std::tuple_size_v<decltype(stats.Interrupts)>);

    std::print("---- {} after {:.0f} s: {} of {} bots connected, {} closed\n", title, seconds, uint32(stats.Open), bots, uint32(stats.Closed));
    std::print("casts sent {}, refused:", uint64(stats.CastsSent));
    for (std::size_t i = 1; i < std::size(CastErrors); ++i)
        if (uint64 count = stats.CastFailed[i])
            std::print(" {} {}", CastErrors[i], count);
    std::print("\nevents by their casters:");
    for (std::size_t i = 0; i < std::size(EventTypes); ++i)
        std::print(" {} {}", EventTypes[i], uint64(stats.Events[i]));
    std::print("\ninterrupted:");
    for (std::size_t i = 1; i < std::size(Reasons); ++i)
        if (uint64 count = stats.Interrupts[i])
            std::print(" {} {}", Reasons[i], count);
    std::print("\ndamage done {}, combat logs {}\n", uint64(stats.Damage), uint64(stats.CombatLogs));

    double perBot = bots && seconds > 0 ? 1.0 / (bots * seconds) : 0.0;
    std::print("per bot per second: {:.1f} KB reliable (units, combat, chat), {:.1f} KB unreliable (frames), {:.1f} packets\n",
        stats.ReliableBytes * perBot / 1024, stats.UnreliableBytes * perBot / 1024, stats.Packets * perBot);
    std::fflush(stdout);
}
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::print(stderr, "Usage: {} <count> [host] [http port] [seconds] [prefix]\n", argv[0]);
        return 1;
    }

    uint32 count = uint32(std::atoi(argv[1]));
    std::string host = argc > 2 ? argv[2] : "127.0.0.1";
    std::string httpPort = argc > 3 ? argv[3] : "8090";
    int seconds = argc > 4 ? std::atoi(argv[4]) : 30;
    std::string prefix = argc > 5 ? argv[5] : "bot";

    // Logging in hashes the password with Argon2 on the server: a few at a time
    std::vector<Optional<Login>> logins(count);
    {
        std::atomic<uint32> next = 0;
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i)
            threads.emplace_back([&]
            {
                for (uint32 n; (n = next++) < count;)
                    {
                    // At least 3 characters, the server's minimum
                    std::string number = std::to_string(n);
                    logins[n] = LogIn(host, httpPort, prefix + std::string(4 - std::min<std::size_t>(4, number.size()), '0') + number);
                }
            });
        for (std::thread& thread : threads)
            thread.join();
    }

    // A client runs on one thread, many clients share one
    std::size_t threadCount = std::clamp<std::size_t>(std::thread::hardware_concurrency() / 4, 1, 8);
    std::vector<std::unique_ptr<Rooftree::Asio::IoContext>> contexts;
    for (std::size_t i = 0; i < threadCount; ++i)
        contexts.push_back(std::make_unique<Rooftree::Asio::IoContext>());

    std::vector<std::shared_ptr<Bot>> bots;
    for (uint32 n = 0; n < count; ++n)
    {
        if (!logins[n])
            continue;

        Rooftree::Asio::IoContext& context = *contexts[n % threadCount];
        boost::asio::ip::udp::resolver resolver(context);
        boost::asio::ip::udp::endpoint server = *resolver.resolve(boost::asio::ip::udp::v4(), host, std::to_string(logins[n]->KcpPort)).begin();

        auto bot = std::make_shared<Bot>(context, Kcp::Settings(), n % 2 == 0);
        bot->Connect(server, logins[n]->Ticket, logins[n]->Key);
        bots.push_back(std::move(bot));
    }
    std::print("{} bots logged in, {} threads\n", bots.size(), threadCount);

    std::vector<std::thread> threads;
    for (auto& context : contexts)
        threads.emplace_back([&context] { context->run(); });

    Clock::time_point start = Clock::now();
    Clock::time_point end = start + std::chrono::seconds(seconds);
    Clock::time_point nextPrint = start + 5s;
    while (Clock::now() < end && stats.Closed < bots.size())
    {
        std::this_thread::sleep_for(100ms);
        if (Clock::now() >= nextPrint)
        {
            PrintStats("so far", std::chrono::duration<double>(Clock::now() - start).count(), uint32(bots.size()));
            nextPrint += 5s;
        }
    }

    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    for (auto& bot : bots)
        bot->Close();
    for (int i = 0; i < 100 && stats.Closed < bots.size(); ++i)
        std::this_thread::sleep_for(20ms);

    PrintStats("done", elapsed, uint32(bots.size()));

    for (auto& context : contexts)
        context->stop();
    for (std::thread& thread : threads)
        thread.join();
    return 0;
}
