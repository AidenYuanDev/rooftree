/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Client addresses refused for a while (an IP ban, kept in memory). The main thread blocks an address
 * that floods (SessionMgr, after AntiDOS refused it too often); the network threads ask before they
 * take a connection or a request from it, so a flood costs no handshake, no parsing and no answer.
 *
 * An IPv6 address counts by its /64, as sessions are keyed: one subscriber picks any address in it.
 * An IPv4 client of a dual stack listener (::ffff:a.b.c.d) counts as its IPv4 address.
 */

#ifndef ROOFTREE_ADDRESS_BLOCKLIST_H
#define ROOFTREE_ADDRESS_BLOCKLIST_H

#include "Duration.h"
#include <boost/asio/ip/address.hpp>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace Rooftree::Net
{
class AddressBlocklist
{
public:
    // Any thread. A block already there is kept if it lasts longer
    void Block(boost::asio::ip::address const& address, TimePoint until)
    {
        std::scoped_lock lock(_mutex);
        TimePoint& entry = _until[Key(address)];
        entry = std::max(entry, until);
        _count.store(_until.size(), std::memory_order_relaxed);
    }

    // Any thread, for every connection and request: no lock while nothing is blocked
    bool IsBlocked(boost::asio::ip::address const& address, TimePoint now) const
    {
        if (!_count.load(std::memory_order_relaxed))
            return false;

        std::scoped_lock lock(_mutex);
        auto itr = _until.find(Key(address));
        return itr != _until.end() && now < itr->second;
    }

    // Forgets the blocks that ended
    void Prune(TimePoint now)
    {
        if (!_count.load(std::memory_order_relaxed))
            return;

        std::scoped_lock lock(_mutex);
        std::erase_if(_until, [now](auto const& entry) { return entry.second <= now; });
        _count.store(_until.size(), std::memory_order_relaxed);
    }

    std::size_t GetCount() const { return _count.load(std::memory_order_relaxed); }

    // What is blocked for an address: itself, IPv4 for a mapped one, the /64 of an IPv6 one
    static boost::asio::ip::address Key(boost::asio::ip::address const& address)
    {
        if (!address.is_v6())
            return address;

        boost::asio::ip::address_v6 v6 = address.to_v6();
        if (v6.is_v4_mapped())
            return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);

        boost::asio::ip::address_v6::bytes_type bytes = v6.to_bytes();
        std::fill(bytes.begin() + 8, bytes.end(), 0);
        return boost::asio::ip::address_v6(bytes);
    }

private:
    mutable std::mutex _mutex;
    std::unordered_map<boost::asio::ip::address, TimePoint> _until;
    std::atomic<std::size_t> _count = 0;
};
}

#endif // ROOFTREE_ADDRESS_BLOCKLIST_H
