/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Addresses in a format, as to_string() writes them: StringFormat("{}", address).
 */

#ifndef ROOFTREE_IP_ADDRESS_H
#define ROOFTREE_IP_ADDRESS_H

#include <boost/asio/ip/address.hpp>

#include <concepts>
#include <format>

namespace Rooftree::Net {
using boost::asio::ip::make_address;
}

template <typename Address>
  requires std::same_as<Address, boost::asio::ip::address> ||
           std::same_as<Address, boost::asio::ip::address_v4> ||
           std::same_as<Address, boost::asio::ip::address_v6>
struct std::formatter<Address> : std::formatter<std::string_view> {
  auto format(Address const& address, std::format_context& context) const {
    return std::formatter<std::string_view>::format(address.to_string(), context);
  }
};

#endif  // ROOFTREE_IP_ADDRESS_H
