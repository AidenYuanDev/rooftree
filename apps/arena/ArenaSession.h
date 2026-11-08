/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * One logged in connection. Created by ArenaSocket once the
 * WebSocket handshake presented a valid ticket (or by ArenaKcpConnection once a KCP Hello did), then
 * owned by World and updated on the world thread. It sends through an ArenaConnection, it doesn't
 * know which transport that is.
 *
 * Messages are FlatBuffers packets (ArenaPackets.h). The network thread reads and verifies them and
 * queues them here (QueueMessage); Update() runs them on the world thread through a handler table:
 * each opcode names the handler and the session status it is valid in.
 */

#ifndef ARENA_SESSION_H
#define ARENA_SESSION_H

#include "ArenaPackets.h"
#include "AsyncCallbackProcessor.h"
#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "MPSCQueue.h"
#include "QueryCallback.h"
#include "WebSocketFrame.h"
#include <atomic>
#include <memory>
#include <string>

class ArenaConnection;
class Player;

enum SessionStatus
{
    STATUS_LOADING,     // waiting for the player's row
    STATUS_IN_GAME
};

class ArenaSession
{
public:
    ArenaSession(uint32 accountId, std::string name, std::shared_ptr<ArenaConnection> connection);
    ~ArenaSession();

    ArenaSession(ArenaSession const&) = delete;
    ArenaSession& operator=(ArenaSession const&) = delete;

    uint32 GetAccountId() const { return _accountId; }
    std::string const& GetName() const { return _name; }
    SessionStatus GetStatus() const { return _status; }
    Player* GetPlayer() const { return _player.get(); }

    // Network thread. False if the client floods us, the caller closes the connection
    bool QueueMessage(std::unique_ptr<ClientMessage> message);

    // World thread from here on

    // Runs queued messages and database callbacks. False once the session is to be removed
    bool Update(uint32 diff);

    void LoadPlayer();
    // Relog: the player of the account's previous session
    void TakeOverPlayer(std::unique_ptr<Player> player);
    std::unique_ptr<Player> ReleasePlayer();

    // A broadcast builds its packet once for everyone. unreliable: may be lost (KCP)
    void Send(ServerPacket const& packet, bool unreliable = false);

    // Closes the connection with a reason the client shows; Update() returns false afterwards
    void KickPlayer(std::string_view reason, Rooftree::Net::WebSocket::CloseCode code = Rooftree::Net::WebSocket::CloseCode::PolicyViolation);
    bool IsKicked() const { return _kicked; }
    bool IsSocketOpen() const;

    void HandlePing(Arena::Ping const& ping);
    void HandleInput(Arena::Input const& input);
    void HandleChat(Arena::Chat const& chat);

private:
    void OnPlayerLoaded(std::unique_ptr<Player> player);

    uint32 _accountId;
    std::string _name;
    std::shared_ptr<ArenaConnection> _connection;
    std::unique_ptr<Player> _player;
    SessionStatus _status = STATUS_LOADING;
    bool _kicked = false;

    MPSCQueue<ClientMessage> _recvQueue;
    std::atomic<uint32> _queuedMessages = 0;
    QueryCallbackProcessor _queryProcessor;

    uint32 _idleTime = 0;
    uint32 _chatCooldown = 0;   // ms until the next chat line is accepted
};

#endif // ARENA_SESSION_H
