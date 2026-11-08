/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Login tickets, the bnetserver way: the HTTP login checks the password and hands out a random ticket,
 * the client presents it once when it opens the WebSocket (ws://host/arena?ticket=...). Browsers can't
 * set headers on a WebSocket handshake, so the query string is where it goes.
 *
 * A native client may open a KCP connection instead (ArenaKcp.h): its Hello carries the ticket and is
 * sealed with the ticket's KcpKey, a random key the login answered with along with the ticket.
 *
 * Kept in memory: a restart only forces clients to log in again. Thread safe.
 */

#ifndef ARENA_TICKET_MGR_H
#define ARENA_TICKET_MGR_H

#include "Define.h"
#include "Duration.h"
#include "KcpProtocol.h"
#include "Optional.h"
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

struct LoginTicket
{
    uint32 AccountId = 0;
    std::string Name;
    Rooftree::Net::Kcp::Key KcpKey = {};     // Create makes it
};

class TicketMgr
{
public:
    static TicketMgr* instance();

    // A ticket valid for lifetime, usable once. Sets its KcpKey
    std::string Create(LoginTicket& ticket, Seconds lifetime);

    // The ticket's account, nullopt if unknown, used or expired
    Optional<LoginTicket> Consume(std::string_view ticket);
    // The same, but only used up if accept(it) agrees (a KCP Hello sealed with its key)
    Optional<LoginTicket> Consume(std::string_view ticket, std::function<bool(LoginTicket const&)> const& accept);

private:
    TicketMgr() = default;

    struct Entry
    {
        LoginTicket Ticket;
        TimePoint Expires;
    };

    std::mutex _lock;
    std::unordered_map<std::string, Entry> _tickets;
};

#define sTicketMgr TicketMgr::instance()

#endif // ARENA_TICKET_MGR_H
