/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "TicketMgr.h"
#include "CryptoRandom.h"
#include "Util.h"

TicketMgr* TicketMgr::instance()
{
    static TicketMgr instance;
    return &instance;
}

std::string TicketMgr::Create(LoginTicket& ticket, Seconds lifetime)
{
    std::string id = Rooftree::ToHex(Rooftree::Crypto::GetRandomBytes<32>());
    ticket.KcpKey = Rooftree::Crypto::GetRandomBytes<Rooftree::Net::Kcp::KeySize>();
    TimePoint now = std::chrono::steady_clock::now();

    std::scoped_lock lock(_lock);

    // Unused tickets are only ever looked up by their id, drop the expired ones here
    std::erase_if(_tickets, [now](auto const& pair) { return pair.second.Expires <= now; });

    _tickets[id] = { .Ticket = ticket, .Expires = now + lifetime };
    return id;
}

Optional<LoginTicket> TicketMgr::Consume(std::string_view ticket)
{
    return Consume(ticket, [](LoginTicket const&) { return true; });
}

Optional<LoginTicket> TicketMgr::Consume(std::string_view ticket, std::function<bool(LoginTicket const&)> const& accept)
{
    std::scoped_lock lock(_lock);

    auto itr = _tickets.find(std::string(ticket));
    if (itr == _tickets.end() || !accept(itr->second.Ticket))
        return {};

    Entry entry = std::move(itr->second);
    _tickets.erase(itr);

    if (entry.Expires <= std::chrono::steady_clock::now())
        return {};

    return std::move(entry.Ticket);
}
