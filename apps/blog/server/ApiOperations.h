/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Every operation of the API, once: its opcode (schema/Admin.fbs, schema/Reader.fbs), who may call
 * it, its HTTP route and the ApiOp doing it (ApiCall.h). The front ends are made from this table, so
 * they can't drift apart:
 *
 *   HTTP     RegisterRoutes adds a route per operation (BlogRoutes.cpp)
 *   packets  RegisterApiPackets an opcode per operation (ApiPackets.h)
 *   console  its commands call the same ApiOps (ConsoleAdmin.cpp)
 *
 * A new admin feature is a row here, its tables in Admin.fbs and a console command (with their tests);
 * a reader's, a row and its tables in Reader.fbs.
 */

#ifndef BLOG_API_OPERATIONS_H
#define BLOG_API_OPERATIONS_H

#include "ApiCall.h"
#include "Admin_generated.h"
#include "HttpService.h"
#include "Reader_generated.h"
#include <boost/beast/http/verb.hpp>
#include <span>
#include <string_view>

namespace Blog
{
// Who may call an operation. A login that is banned may call none but UserEvenBanned's
enum class ApiAccess
{
    Anyone,             // with a login or without
    User,
    UserEvenBanned,     // what a banned account may still do: read itself, log out
    Moderator,
    Admin
};

// An opcode of either schema's enum
struct ApiOpcode
{
    constexpr ApiOpcode(Admin::Opcode opcode) : Value(uint16(opcode)) { }
    constexpr ApiOpcode(Reader::Opcode opcode) : Value(uint16(opcode)) { }

    uint16 Value;
};

struct ApiOperation
{
    ApiOpcode Opcode;
    ApiAccess Access = ApiAccess::Admin;
    // Over HTTP a login's changes are checked by its CSRF token; an anonymous one's must come from our
    // Origin (a packet's connection was checked so)
    bool CheckOrigin = false;
    boost::beast::http::verb Method;
    std::string_view Path;                  // its {name} segments are fields of the request table
    ApiOp Op = nullptr;
    std::string_view Request;               // the packet's root type, "Blog.Admin.PostInput"
    std::string_view Response;
    std::string_view MessageType = "admin"; // for AntiDOS and logs, shared by its route and its packet
    Rooftree::Net::Http::RequestHandlerFlag Flags = Rooftree::Net::Http::RequestHandlerFlag::None;
};

// "PostsList": the opcode's name in its enum
std::string_view GetOpcodeName(uint16 opcode);

std::span<ApiOperation const> GetApiOperations();

// None for an opcode that isn't an operation's
ApiOperation const* FindApiOperation(uint16 opcode);

// The operation of op, which is one (asserted)
ApiOperation const& GetApiOperation(ApiOp op);
}

#endif // BLOG_API_OPERATIONS_H
