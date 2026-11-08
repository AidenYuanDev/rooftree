/*
 * A native arena client over KCP, what a game client (Unreal, Unity) does in a few lines:
 *
 *   1. POST /api/login: the ticket, and kcpPort + kcpKey (hex) when the server has Arena.Kcp on
 *   2. Kcp::Client::Connect with the ticket as the token and the key
 *   3. packets: an 8 byte header [uint16 opcode][uint16 flags][uint32 requestId] (little-endian, 0
 *      for both) and the opcode's table of apps/arena/schema/Arena.fbs, which flatc turns into code
 *      for C++ (this, Unreal), C# (Unity) and more. The state comes unreliably
 *
 * Usage: arena_kcp_client <username> <password> [host] [http port] [seconds]
 * Logs in, says hello in the chat, walks right and prints what it hears, then leaves.
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
#include <fmt/core.h>
#include <atomic>
#include <thread>

using namespace std::chrono_literals;
namespace http = boost::beast::http;
namespace Kcp = Rooftree::Net::Kcp;

namespace
{
constexpr std::size_t PacketHeaderSize = 8;

std::string MakePacket(Arena::Opcode opcode, flatbuffers::FlatBufferBuilder const& builder)
{
    std::string packet(PacketHeaderSize, '\0');
    packet[0] = char(uint16(opcode) & 0xFF);
    packet[1] = char(uint16(opcode) >> 8);
    packet.append(reinterpret_cast<char const*>(builder.GetBufferPointer()), builder.GetSize());
    return packet;
}

// A field the server left out is null, as any table's
std::string_view Str(flatbuffers::String const* string)
{
    return string ? string->string_view() : std::string_view();
}

double Now()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class ArenaClient final : public Kcp::Client
{
public:
    using Client::Client;

    std::atomic<bool> Done = false;
    std::atomic<uint32> States = 0;

protected:
    void OnOpen() override
    {
        fmt::print("connected, conv {}\n", GetConv());
    }

    void OnMessage(std::string&& packet, bool reliable) override
    {
        if (packet.size() < PacketHeaderSize)
            return;

        // Copied to an 8-aligned buffer: the verifier checks alignment, and a table is read in place.
        // Verified before anything reads it, as a server's packet is no more trusted than any input
        Arena::Opcode opcode = Arena::Opcode(uint8(packet[0]) | uint8(packet[1]) << 8);
        std::vector<uint8> payload(packet.begin() + PacketHeaderSize, packet.end());
        flatbuffers::Verifier verifier(payload.data(), payload.size());
        char const* tag = reliable ? "reliable  " : "unreliable";

        switch (opcode)
        {
            case Arena::Opcode::Welcome:
            {
                if (!verifier.VerifyBuffer<Arena::Welcome>(nullptr))
                    break;
                Arena::Welcome const* welcome = flatbuffers::GetRoot<Arena::Welcome>(payload.data());
                fmt::print("{} Welcome {} (id {}), field {}x{}, {} coins so far\n", tag, Str(welcome->name()), welcome->id(),
                    welcome->width(), welcome->height(), welcome->totalCoins());

                // In game: the server ignores inputs and chat before
                flatbuffers::FlatBufferBuilder builder;
                builder.Finish(Arena::CreateChatDirect(builder, "hello over KCP"));
                Send(MakePacket(Arena::Opcode::Chat, builder));

                builder.Clear();
                builder.Finish(Arena::CreateInput(builder, 1, 0));
                Send(MakePacket(Arena::Opcode::Input, builder));

                builder.Clear();
                builder.Finish(Arena::CreatePing(builder, Now()));
                Send(MakePacket(Arena::Opcode::Ping, builder));
                break;
            }
            case Arena::Opcode::State:
            {
                // 20 states a second, print the first and then one a second
                if (!verifier.VerifyBuffer<Arena::State>(nullptr) || States++ % 20)
                    break;
                Arena::State const* state = flatbuffers::GetRoot<Arena::State>(payload.data());
                fmt::print("{} State, {} bytes:", tag, packet.size());
                if (state->players())
                    for (Arena::PlayerState const* player : *state->players())
                        fmt::print(" {} at ({:.0f}, {:.0f})", Str(player->name()), player->x(), player->y());
                fmt::print(", {} coins{}\n", state->coins() ? state->coins()->size() : 0, state->goldRush() ? ", gold rush" : "");
                break;
            }
            case Arena::Opcode::ChatMessage:
                if (verifier.VerifyBuffer<Arena::ChatMessage>(nullptr))
                {
                    Arena::ChatMessage const* chat = flatbuffers::GetRoot<Arena::ChatMessage>(payload.data());
                    fmt::print("{} ChatMessage {}: {}\n", tag, Str(chat->from()), Str(chat->text()));
                }
                break;
            case Arena::Opcode::SystemMessage:
                if (verifier.VerifyBuffer<Arena::SystemMessage>(nullptr))
                    fmt::print("{} SystemMessage {}\n", tag, Str(flatbuffers::GetRoot<Arena::SystemMessage>(payload.data())->text()));
                break;
            case Arena::Opcode::Pong:
                if (verifier.VerifyBuffer<Arena::Pong>(nullptr))
                    fmt::print("{} Pong, {:.1f} ms\n", tag, Now() - flatbuffers::GetRoot<Arena::Pong>(payload.data())->t());
                break;
            default:
                // A newer server's: ignored
                break;
        }
    }

    void OnClose(uint16 code, std::string_view reason) override
    {
        fmt::print("closed: {} {}\n", code, reason);
        Done = true;
    }
};
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "Usage: {} <username> <password> [host] [http port] [seconds]\n", argv[0]);
        return 1;
    }

    std::string host = argc > 3 ? argv[3] : "127.0.0.1";
    std::string httpPort = argc > 4 ? argv[4] : "8090";
    int seconds = argc > 5 ? std::atoi(argv[5]) : 5;

    // 1. Log in. A real client does this over HTTPS: the key must stay secret
    boost::json::object login;
    try
    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::resolver resolver(io);
        boost::asio::ip::tcp::socket socket(io);
        boost::asio::connect(socket, resolver.resolve(host, httpPort));

        http::request<http::string_body> request(http::verb::post, "/api/login", 11);
        request.set(http::field::host, host);
        request.set(http::field::content_type, "application/json");
        request.body() = boost::json::serialize(boost::json::object{ { "username", argv[1] }, { "password", argv[2] } });
        request.prepare_payload();
        http::write(socket, request);

        boost::beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(socket, buffer, response);
        login = boost::json::parse(response.body()).as_object();
        if (response.result() != http::status::ok)
        {
            fmt::print(stderr, "login failed: {}\n", response.body());
            return 1;
        }
    }
    catch (std::exception const& e)
    {
        fmt::print(stderr, "login failed: {}\n", e.what());
        return 1;
    }

    if (!login.contains("kcpPort"))
    {
        fmt::print(stderr, "the server has KCP off (Arena.Kcp.Enabled)\n");
        return 1;
    }

    std::string ticket(login.at("ticket").as_string());
    std::vector<uint8> keyBytes = Rooftree::FromHex(login.at("kcpKey").as_string()).value_or(std::vector<uint8>());
    Kcp::Key key;
    if (keyBytes.size() != key.size())
    {
        fmt::print(stderr, "unexpected key size {}\n", keyBytes.size());
        return 1;
    }
    std::copy(keyBytes.begin(), keyBytes.end(), key.begin());

    // 2. Connect over UDP
    Rooftree::Asio::IoContext ioContext;
    boost::asio::ip::udp::resolver resolver(ioContext);
    boost::asio::ip::udp::endpoint server = *resolver.resolve(boost::asio::ip::udp::v4(), host, std::to_string(login.at("kcpPort").as_int64())).begin();

    // Pinging every second instead of every 5 shows the round trip sooner
    Kcp::Settings settings;
    settings.PingInterval = 1s;
    auto client = std::make_shared<ArenaClient>(ioContext, settings);
    client->Connect(server, ticket, key);
    std::thread ioThread([&ioContext] { ioContext.run(); });

    // 3. Play for a while, then leave
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!client->Done && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(50ms);

    if (!client->Done)
    {
        fmt::print("round trip {} ms, {} states received, leaving\n", client->GetRoundTripTime(), uint32(client->States));
        client->Close();
        while (!client->Done)
            std::this_thread::sleep_for(10ms);
    }

    ioContext.stop();
    ioThread.join();
    return 0;
}
