/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "ArenaSession.h"
#include "ArenaDatabase.h"
#include "ArenaConnection.h"
#include "Field.h"
#include "Log.h"
#include "Player.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "Random.h"
#include "Util.h"
#include "World.h"
#include <utf8.h>
#include <unordered_map>

namespace
{
// Unanswered messages a client may have queued before it is disconnected, the world thread runs them
// all every tick, so a well-behaved client never has more than a handful
constexpr uint32 MaxQueuedMessages = 200;

// The opcode table: opcode -> status it is valid in + handler, which gets the opcode's table
struct MessageHandler
{
    SessionStatus Status;
    void (*Call)(ArenaSession& session, ClientMessage const& message);
};

template <class Table, void (ArenaSession::*Handler)(Table const&)>
void CallHandler(ArenaSession& session, ClientMessage const& message)
{
    (session.*Handler)(message.Get<Table>());
}

std::unordered_map<Arena::Opcode, MessageHandler> const MessageHandlers =
{
    { Arena::Opcode::Ping,  { STATUS_LOADING, &CallHandler<Arena::Ping, &ArenaSession::HandlePing> } },
    { Arena::Opcode::Input, { STATUS_IN_GAME, &CallHandler<Arena::Input, &ArenaSession::HandleInput> } },
    { Arena::Opcode::Chat,  { STATUS_IN_GAME, &CallHandler<Arena::Chat, &ArenaSession::HandleChat> } },
};

std::string_view Trim(std::string_view text)
{
    std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos)
        return {};
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}
}

ArenaSession::ArenaSession(uint32 accountId, std::string name, std::shared_ptr<ArenaConnection> connection)
    : _accountId(accountId), _name(std::move(name)), _connection(std::move(connection))
{
}

ArenaSession::~ArenaSession()
{
    if (_connection && _connection->IsConnectionOpen())
        _connection->Disconnect(Rooftree::Net::WebSocket::CloseCode::Normal, {});
}

bool ArenaSession::QueueMessage(std::unique_ptr<ClientMessage> message)
{
    if (++_queuedMessages > MaxQueuedMessages)
        return false;

    _recvQueue.Enqueue(message.release());
    return true;
}

bool ArenaSession::Update(uint32 diff)
{
    // The connection closed (client left, kicked, network error): World removes us
    if (!_connection->IsConnectionOpen() || _kicked)
        return false;

    _idleTime += diff;
    _chatCooldown -= std::min(_chatCooldown, diff);

    ClientMessage* raw;
    while (!_kicked && _recvQueue.Dequeue(raw))
    {
        std::unique_ptr<ClientMessage> message(raw);
        --_queuedMessages;
        _idleTime = 0;

        // ArenaPackets.cpp's client opcodes, each has one
        auto itr = MessageHandlers.find(message->Opcode);
        if (itr == MessageHandlers.end())
        {
            RT_LOG_ERROR("arena.session", "{}: no handler for {}", _name, GetOpcodeName(message->Opcode));
            continue;
        }

        // STATUS_LOADING handlers are valid in game too
        if (itr->second.Status == STATUS_IN_GAME && _status != STATUS_IN_GAME)
        {
            RT_LOG_DEBUG("arena.session", "{}: {} before the player is loaded, ignored", _name, GetOpcodeName(message->Opcode));
            continue;
        }

        itr->second.Call(*this, *message);
    }

    _queryProcessor.ProcessReadyCallbacks();

    if (Milliseconds(_idleTime) > sWorld->GetSettings().IdleTimeout)
    {
        RT_LOG_INFO("arena.session", "{} timed out", _name);
        KickPlayer("timed out");
        return false;
    }

    return !_kicked;
}

void ArenaSession::LoadPlayer()
{
    ArenaDatabasePreparedStatement* stmt = ArenaDatabase.GetPreparedStatement(ARENA_SEL_PLAYER);
    stmt->setUInt32(0, _accountId);

    // The callback runs on the world thread, in Update() -> ProcessReadyCallbacks(), and never after
    // the session is gone: the processor is a member
    _queryProcessor.AddCallback(ArenaDatabase.AsyncQuery(stmt).WithPreparedCallback([this](PreparedQueryResult result)
    {
        if (!result)
        {
            RT_LOG_ERROR("arena.session", "{} (account {}) has no player row", _name, _accountId);
            KickPlayer("account has no player");
            return;
        }

        Field* fields = result->Fetch();
        OnPlayerLoaded(std::make_unique<Player>(_accountId, _name, fields[0].GetUInt32(), fields[1].GetUInt32()));
    }));

    stmt = ArenaDatabase.GetPreparedStatement(ARENA_UPD_LAST_LOGIN);
    stmt->setUInt32(0, _accountId);
    ArenaDatabase.Execute(stmt);
}

void ArenaSession::TakeOverPlayer(std::unique_ptr<Player> player)
{
    OnPlayerLoaded(std::move(player));
}

std::unique_ptr<Player> ArenaSession::ReleasePlayer()
{
    return std::move(_player);
}

void ArenaSession::OnPlayerLoaded(std::unique_ptr<Player> player)
{
    WorldSettings const& settings = sWorld->GetSettings();

    _player = std::move(player);
    _player->SetInput(0, 0);
    _player->Relocate(frand(Player::Radius, settings.Width - Player::Radius), frand(Player::Radius, settings.Height - Player::Radius));
    _status = STATUS_IN_GAME;

    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Arena::CreateWelcomeDirect(builder, _accountId, _name.c_str(), settings.Width, settings.Height, Player::Radius,
        _player->GetTotalCoins(), settings.Motd.c_str()));
    Send({ Arena::Opcode::Welcome, builder });

    sWorld->SendSystemMessage(Rooftree::StringFormat("{} joined", _name));
}

void ArenaSession::Send(ServerPacket const& packet, bool unreliable /*= false*/)
{
    _connection->Write(packet, unreliable);
}

void ArenaSession::KickPlayer(std::string_view reason, Rooftree::Net::WebSocket::CloseCode code /*= PolicyViolation*/)
{
    if (_kicked)
        return;

    _kicked = true;
    _connection->Disconnect(code, reason);
}

bool ArenaSession::IsSocketOpen() const
{
    return _connection->IsConnectionOpen();
}

void ArenaSession::HandlePing(Arena::Ping const& ping)
{
    // Echo the client's timestamp so it can show its round trip time
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Arena::CreatePong(builder, ping.t()));
    Send({ Arena::Opcode::Pong, builder });
}

void ArenaSession::HandleInput(Arena::Input const& input)
{
    _player->SetInput(std::clamp<int32>(input.dx(), -1, 1), std::clamp<int32>(input.dy(), -1, 1));
}

void ArenaSession::HandleChat(Arena::Chat const& chat)
{
    if (!chat.text())
        return;

    // A packet's string may be any bytes, the page's JSON is UTF-8 already
    std::string text(Trim(chat.text()->string_view()));
    if (text.empty() || !utf8::is_valid(text.begin(), text.end()))
        return;

    WorldSettings const& settings = sWorld->GetSettings();
    if (_chatCooldown)
    {
        Send(MakeSystemMessage("You are sending messages too fast."));
        return;
    }
    _chatCooldown = uint32(settings.ChatInterval.count());

    Rooftree::Utf8Truncate(text, settings.MaxChatLength);
    RT_LOG_INFO("arena.chat", "{}: {}", _name, text);
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Arena::CreateChatMessageDirect(builder, _name.c_str(), text.c_str()));
    sWorld->SendGlobalMessage({ Arena::Opcode::ChatMessage, builder });
}
