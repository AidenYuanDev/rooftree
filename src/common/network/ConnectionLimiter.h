/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * How many connections one address may have open at once, so a single client can't
 * take all the file descriptors and memory with thousands of idle sockets. SocketMgr asks before it
 * creates a socket and gives the socket the ticket; the count goes down when the socket is destroyed,
 * whichever way it closed.
 *
 * Addresses that front many clients (a reverse proxy, a load balancer) are exempt: every connection
 * from them would count as the same client.
 *
 * It also refuses the addresses of an AddressBlocklist (SetBlocklist), but never the exempt ones: a
 * proxy's clients are blocked by the address it forwards, request by request.
 */

#ifndef ROOFTREE_CONNECTION_LIMITER_H
#define ROOFTREE_CONNECTION_LIMITER_H

#include "AddressBlocklist.h"
#include "Define.h"
#include <boost/asio/ip/address.hpp>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Rooftree::Net
{
class ConnectionLimiter
{
public:
    ConnectionLimiter() : _state(std::make_shared<State>()) { }

    // Before the network starts. 0 is no limit
    void SetLimit(uint32 maxPerAddress, std::vector<boost::asio::ip::address> exempt = {})
    {
        _maxPerAddress = maxPerAddress;
        _exempt = std::move(exempt);
        for (boost::asio::ip::address& address : _exempt)
            address = Normalize(address);
    }

    uint32 GetLimit() const { return _maxPerAddress; }

    // Before the network starts
    void SetBlocklist(std::shared_ptr<AddressBlocklist const> blocklist) { _blocklist = std::move(blocklist); }

    // Any thread. Blocked, and not exempt
    bool IsBlocked(boost::asio::ip::address const& peer) const
    {
        if (!_blocklist)
            return false;

        boost::asio::ip::address address = Normalize(peer);
        return std::ranges::find(_exempt, address) == _exempt.end() && _blocklist->IsBlocked(address, TimePoint::clock::now());
    }

    // Any thread. False if the address has its maximum open already. Otherwise ticket is what the
    // socket keeps until it is destroyed (empty when nothing is counted)
    bool TryAcquire(boost::asio::ip::address const& peer, std::shared_ptr<void>& ticket) const
    {
        if (!_maxPerAddress)
            return true;

        boost::asio::ip::address address = Normalize(peer);
        if (std::ranges::find(_exempt, address) != _exempt.end())
            return true;

        {
            std::scoped_lock lock(_state->Mutex);
            uint32& count = _state->Counts[address];
            if (count >= _maxPerAddress)
                return false;
            ++count;
        }

        // The state is shared: a socket may outlive the SocketMgr that accepted it
        ticket = std::shared_ptr<void>(nullptr, [state = _state, address](void*)
        {
            std::scoped_lock lock(state->Mutex);
            auto itr = state->Counts.find(address);
            if (itr != state->Counts.end() && --itr->second == 0)
                state->Counts.erase(itr);
        });
        return true;
    }

    // Any thread
    uint32 GetCount(boost::asio::ip::address const& peer) const
    {
        std::scoped_lock lock(_state->Mutex);
        auto itr = _state->Counts.find(Normalize(peer));
        return itr != _state->Counts.end() ? itr->second : 0;
    }

    // IPv4 clients of a dual stack listener arrive as ::ffff:a.b.c.d
    static boost::asio::ip::address Normalize(boost::asio::ip::address const& address)
    {
        if (address.is_v6() && address.to_v6().is_v4_mapped())
            return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6());
        return address;
    }

private:
    struct State
    {
        std::mutex Mutex;
        std::unordered_map<boost::asio::ip::address, uint32> Counts;
    };

    uint32 _maxPerAddress = 0;
    std::vector<boost::asio::ip::address> _exempt;
    std::shared_ptr<AddressBlocklist const> _blocklist;
    std::shared_ptr<State> _state;
};
}

#endif // ROOFTREE_CONNECTION_LIMITER_H
