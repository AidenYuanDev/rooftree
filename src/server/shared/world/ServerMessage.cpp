/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "ServerMessage.h"
#include <algorithm>

namespace Rooftree::Server
{
MessageType const& MessageTypeRegistry::Register(std::string_view name, std::string_view sessionClass)
{
    std::scoped_lock lock(_mutex);
    auto itr = std::ranges::find(_types, name, &MessageType::Name);
    if (itr != _types.end())
        return *itr;

    return _types.emplace_back(MessageType{ .Id = uint32(_types.size()), .Name = std::string(name), .SessionClass = std::string(sessionClass) });
}

MessageType const* MessageTypeRegistry::Find(std::string_view name) const
{
    std::scoped_lock lock(_mutex);
    auto itr = std::ranges::find(_types, name, &MessageType::Name);
    return itr != _types.end() ? &*itr : nullptr;
}

std::size_t MessageTypeRegistry::GetCount() const
{
    std::scoped_lock lock(_mutex);
    return _types.size();
}
}
