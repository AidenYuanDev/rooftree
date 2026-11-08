/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Every client message runs on the main thread: a network thread only parses it and queues it on its
 * session, and SessionMgr runs a bounded number of them per session and update, each through AntiDOS
 * first. A Message is whatever a network thread hands to the main thread, an HTTP request
 * (WorldHttpService), a WebSocket message (WorldWebSocket) or later a game packet.
 *
 * Every message has a type, like an opcode. Types are registered by name at startup (an HTTP route
 * names its type, "comment.create"), AntiDOS limits are configured per type name, and per-session
 * counters are indexed by the type's id.
 */

#ifndef ROOFTREE_SERVER_MESSAGE_H
#define ROOFTREE_SERVER_MESSAGE_H

#include "Define.h"
#include <boost/asio/ip/address.hpp>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

namespace Rooftree::Server
{
class Session;

struct MessageType
{
    uint32 Id;
    std::string Name;
    std::string SessionClass;   // of the sessions it starts (SessionMgr), "" the default class
};

// Names to types, filled before the network starts and only read afterwards. Types never move, so
// messages keep plain references to them
class MessageTypeRegistry
{
public:
    // The existing type for a name registered before. sessionClass: the class of a session this type's
    // message starts (SessionMgr), "" the default one
    MessageType const& Register(std::string_view name, std::string_view sessionClass = {});

    MessageType const* Find(std::string_view name) const;
    std::size_t GetCount() const;

    template<typename Callable>
    void ForEach(Callable&& callable) const
    {
        std::scoped_lock lock(_mutex);
        for (MessageType const& type : _types)
            callable(type);
    }

private:
    mutable std::mutex _mutex;
    std::deque<MessageType> _types;
};

enum class RejectReason
{
    RateLimited,    // AntiDOS refused it
    Overloaded,     // the server's or the session's queue is full
    SessionClosed,  // the session went away before running it
    Blocked         // its address is blocked (AntiDosSettings::BlockAfter): drop the connection, answer nothing
};

class Message
{
public:
    Message(MessageType const& type, std::string sessionKey, boost::asio::ip::address remoteAddress)
        : _type(&type), _sessionKey(std::move(sessionKey)), _remoteAddress(std::move(remoteAddress)) { }

    Message(Message const&) = delete;
    Message& operator=(Message const&) = delete;
    virtual ~Message() = default;

    MessageType const& GetType() const { return *_type; }
    std::string const& GetSessionKey() const { return _sessionKey; }
    boost::asio::ip::address const& GetRemoteAddress() const { return _remoteAddress; }

    // Main thread, from SessionMgr::Update, after AntiDOS let it through
    virtual void Process(Session& session) = 0;

    // Any thread: it will never be processed. An HTTP message answers 429/503 here
    virtual void Reject(RejectReason reason) = 0;

    std::atomic<Message*> QueueLink = nullptr;  // for SessionMgr's inbox

private:
    MessageType const* _type;
    std::string _sessionKey;
    boost::asio::ip::address _remoteAddress;
};
}

#endif // ROOFTREE_SERVER_MESSAGE_H
