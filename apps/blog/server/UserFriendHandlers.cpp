/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * User friendship (social) operations: friend requests, accept/reject, friend list with online status,
 * removing friends, and checking friendship status with another user.
 */

#include "ApiCall.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Field.h"
#include "PreparedStatement.h"
#include "Presence.h"
#include "QueryResult.h"
#include "Reader_generated.h"
#include "Transaction.h"
#include <boost/lexical_cast.hpp>

namespace Blog
{

using boost::beast::http::status;
using Rooftree::Server::Session;

struct FriendItemData
{
    uint32 Id = 0;
    std::string Username;
    std::string DisplayName;
    std::string Avatar;
    SystemTimePoint Since;
};

struct FriendRequestData
{
    uint32 Id = 0;
    FriendItemData User;
    std::string Message;
    SystemTimePoint CreatedAt;
};

// GET /api/user/friends
Rooftree::Task<void> ListUserFriends(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;

    std::vector<FriendItemData> friends = co_await session.OnWorker([callerId]()
    {
        std::vector<FriendItemData> friends;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIENDS);
        stmt->setUInt32(0, callerId);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                friends.push_back({
                    fields[0].GetUInt32(),
                    fields[1].GetString(),
                    fields[2].GetString(),
                    fields[3].GetStringOrNull().value_or(""),
                    fields[4].GetDate()
                });
            } while (rows->NextRow());
        }
        return friends;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::UserFriendItem>> items;
    items.reserve(friends.size());

    for (FriendItemData const& f : friends)
    {
        bool online = sWorld->GetPresence().FindOnlineUser(f.Id).has_value();
        auto avatar = f.Avatar.empty() ? 0 : builder.CreateString("/uploads/" + f.Avatar);
        items.push_back(Reader::CreateUserFriendItem(
            builder,
            f.Id,
            builder.CreateString(f.Username),
            builder.CreateString(f.DisplayName),
            avatar,
            online,
            TimeString(builder, f.Since)
        ));
    }

    builder.Finish(Reader::CreateUserFriendsList(builder, builder.CreateVector(items)));
    request->Respond(builder);
}

// GET /api/user/friends/requests
Rooftree::Task<void> ListUserFriendRequests(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;

    std::pair<std::vector<FriendRequestData>, std::vector<FriendRequestData>> data = co_await session.OnWorker([callerId]()
    {
        std::vector<FriendRequestData> incoming;
        std::vector<FriendRequestData> outgoing;

        // Incoming requests (sent to caller)
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIEND_REQUESTS_IN);
        stmt->setUInt32(0, callerId);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                incoming.push_back({
                    fields[0].GetUInt32(),
                    {
                        fields[1].GetUInt32(),
                        fields[2].GetString(),
                        fields[3].GetString(),
                        fields[4].GetStringOrNull().value_or(""),
                        fields[6].GetDate()
                    },
                    fields[5].GetString(),
                    fields[6].GetDate()
                });
            } while (rows->NextRow());
        }

        // Outgoing requests (sent by caller)
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIEND_REQUESTS_OUT);
        stmt->setUInt32(0, callerId);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                outgoing.push_back({
                    fields[0].GetUInt32(),
                    {
                        fields[1].GetUInt32(),
                        fields[2].GetString(),
                        fields[3].GetString(),
                        fields[4].GetStringOrNull().value_or(""),
                        fields[6].GetDate()
                    },
                    fields[5].GetString(),
                    fields[6].GetDate()
                });
            } while (rows->NextRow());
        }

        return std::make_pair(std::move(incoming), std::move(outgoing));
    });
    auto& [incoming, outgoing] = data;
    flatbuffers::FlatBufferBuilder builder;

    auto makeItems = [&](std::vector<FriendRequestData> const& reqs)
    {
        std::vector<flatbuffers::Offset<Reader::UserFriendRequestItem>> items;
        items.reserve(reqs.size());
        for (FriendRequestData const& r : reqs)
        {
            bool online = sWorld->GetPresence().FindOnlineUser(r.User.Id).has_value();
            auto avatar = r.User.Avatar.empty() ? 0 : builder.CreateString("/uploads/" + r.User.Avatar);
            auto userItem = Reader::CreateUserFriendItem(
                builder,
                r.User.Id,
                builder.CreateString(r.User.Username),
                builder.CreateString(r.User.DisplayName),
                avatar,
                online,
                TimeString(builder, r.User.Since)
            );
            items.push_back(Reader::CreateUserFriendRequestItem(
                builder,
                r.Id,
                userItem,
                builder.CreateString(r.Message),
                TimeString(builder, r.CreatedAt)
            ));
        }
        return builder.CreateVector(items);
    };

    auto inVec = makeItems(incoming);
    auto outVec = makeItems(outgoing);

    builder.Finish(Reader::CreateUserFriendRequests(builder, inVec, outVec));
    request->Respond(builder);
}

// POST /api/user/friends/requests
Rooftree::Task<void> SendUserFriendRequest(Session& session, ApiRequest request)
{
    AuthInfo const& auth = request->GetAuth();
    if (auth.User.IsDenied("friend"))
        co_return request->RespondError(status::forbidden, "not_allowed");

    Reader::UserFriendRequestSendRequest const& in = request->Get<Reader::UserFriendRequestSendRequest>();
    std::string to = in.to() ? in.to()->str() : "";
    std::string message = in.message() ? in.message()->str() : "";

    if (to.empty())
        co_return request->RespondError(status::bad_request, "target_required");
    if (message.size() > 200)
        message.resize(200);

    uint32 callerId = auth.User.Id;

    struct Result
    {
        status StatusCode = status::ok;
        std::string ErrorCode;
        uint32 TargetId = 0;
    };

    Result res = co_await session.OnWorker([callerId, to = std::move(to), message = std::move(message)]() -> Result
    {
        uint32 targetId = 0;
        // Try parsing to as uint32 ID first
        try
        {
            uint32 id = boost::lexical_cast<uint32>(to);
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FOR_FRIEND_BY_ID);
            stmt->setUInt32(0, id);
            if (PreparedQueryResult row = BlogDatabase.Query(stmt))
                targetId = id;
        }
        catch (boost::bad_lexical_cast const&) { }

        // If not found by ID, look up by username
        if (targetId == 0)
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FOR_FRIEND_BY_NAME);
            stmt->setString(0, to);
            if (PreparedQueryResult row = BlogDatabase.Query(stmt))
                targetId = row->Fetch()[0].GetUInt32();
        }

        if (targetId == 0)
            return { status::not_found, "user_not_found", 0 };
        if (targetId == callerId)
            return { status::bad_request, "self_friend", 0 };

        // Check if already friends
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIEND_PAIR);
        stmt->setUInt32(0, callerId);
        stmt->setUInt32(1, targetId);
        if (BlogDatabase.Query(stmt))
            return { status::conflict, "already_friends", targetId };

        // Check if pending request exists
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIEND_REQUEST_PAIR);
        stmt->setUInt32(0, callerId);
        stmt->setUInt32(1, targetId);
        stmt->setUInt32(2, targetId);
        stmt->setUInt32(3, callerId);
        if (PreparedQueryResult row = BlogDatabase.Query(stmt))
        {
            Field* fields = row->Fetch();
            uint32 fromId = fields[1].GetUInt32();
            if (fromId == targetId)
            {
                // Target already requested friendship with caller: auto-accept!
                BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
                stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_USER_FRIEND);
                stmt->setUInt32(0, callerId);
                stmt->setUInt32(1, targetId);
                trans->Append(stmt);

                stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_USER_FRIEND);
                stmt->setUInt32(0, targetId);
                stmt->setUInt32(1, callerId);
                trans->Append(stmt);

                stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_USER_FRIEND_REQUEST_PAIR);
                stmt->setUInt32(0, callerId);
                stmt->setUInt32(1, targetId);
                stmt->setUInt32(2, targetId);
                stmt->setUInt32(3, callerId);
                trans->Append(stmt);

                BlogDatabase.DirectCommitTransaction(trans);
                return { status::ok, "", targetId };
            }
            // Caller already requested
            return { status::conflict, "already_sent", targetId };
        }

        // Insert new friend request
        stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_USER_FRIEND_REQUEST);
        stmt->setUInt32(0, callerId);
        stmt->setUInt32(1, targetId);
        stmt->setString(2, message);
        BlogDatabase.DirectExecute(stmt);

        return { status::ok, "", targetId };
    });
    if (res.StatusCode != status::ok)
        co_return request->RespondError(res.StatusCode, res.ErrorCode);

    sWorld->GetPresence().Changed("user:" + std::to_string(callerId), "friends");
    sWorld->GetPresence().Changed("user:" + std::to_string(res.TargetId), "friends");
    RespondOk(request);
}

// POST /api/user/friends/requests/{id}/accept
Rooftree::Task<void> AcceptUserFriendRequest(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;
    uint32 reqId = request->Get<Reader::UserFriendRequestActionRequest>().id();

    std::pair<bool, uint32> res = co_await session.OnWorker([callerId, reqId]() -> std::pair<bool, uint32>
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIEND_REQUEST_BY_ID);
        stmt->setUInt32(0, reqId);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
            return { false, 0 };

        Field* fields = row->Fetch();
        uint32 fromUserId = fields[1].GetUInt32();
        uint32 toUserId = fields[2].GetUInt32();

        if (toUserId != callerId)
            return { false, 0 };

        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();

        stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_USER_FRIEND);
        stmt->setUInt32(0, callerId);
        stmt->setUInt32(1, fromUserId);
        trans->Append(stmt);

        stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_USER_FRIEND);
        stmt->setUInt32(0, fromUserId);
        stmt->setUInt32(1, callerId);
        trans->Append(stmt);

        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_USER_FRIEND_REQUEST_PAIR);
        stmt->setUInt32(0, callerId);
        stmt->setUInt32(1, fromUserId);
        stmt->setUInt32(2, fromUserId);
        stmt->setUInt32(3, callerId);
        trans->Append(stmt);

        BlogDatabase.DirectCommitTransaction(trans);
        return { true, fromUserId };
    });
    auto [ok, fromUserId] = res;
    if (!ok)
        co_return request->RespondError(status::not_found, "request_not_found");

    sWorld->GetPresence().Changed("user:" + std::to_string(callerId), "friends");
    sWorld->GetPresence().Changed("user:" + std::to_string(fromUserId), "friends");
    RespondOk(request);
}

// DELETE /api/user/friends/requests/{id}
Rooftree::Task<void> RejectUserFriendRequest(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;
    uint32 reqId = request->Get<Reader::UserFriendRequestActionRequest>().id();

    std::pair<bool, uint32> res = co_await session.OnWorker([callerId, reqId]() -> std::pair<bool, uint32>
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIEND_REQUEST_BY_ID);
        stmt->setUInt32(0, reqId);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
            return { false, 0 };

        Field* fields = row->Fetch();
        uint32 fromUserId = fields[1].GetUInt32();
        uint32 toUserId = fields[2].GetUInt32();

        if (toUserId != callerId && fromUserId != callerId)
            return { false, 0 };

        uint32 otherId = (toUserId == callerId) ? fromUserId : toUserId;

        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_USER_FRIEND_REQUEST);
        stmt->setUInt32(0, reqId);
        BlogDatabase.DirectExecute(stmt);

        return { true, otherId };
    });
    auto [ok, otherId] = res;
    if (!ok)
        co_return request->RespondError(status::not_found, "request_not_found");

    sWorld->GetPresence().Changed("user:" + std::to_string(callerId), "friends");
    sWorld->GetPresence().Changed("user:" + std::to_string(otherId), "friends");
    RespondOk(request);
}

// DELETE /api/user/friends/{id}
Rooftree::Task<void> RemoveUserFriend(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;
    uint32 friendId = request->Get<Reader::UserFriendRemoveRequest>().id();

    co_await session.OnWorker([callerId, friendId]()
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_USER_FRIEND);
        stmt->setUInt32(0, callerId);
        stmt->setUInt32(1, friendId);
        stmt->setUInt32(2, friendId);
        stmt->setUInt32(3, callerId);
        BlogDatabase.DirectExecute(stmt);
    });
    sWorld->GetPresence().Changed("user:" + std::to_string(callerId), "friends");
    sWorld->GetPresence().Changed("user:" + std::to_string(friendId), "friends");
    RespondOk(request);
}

// GET /api/user/friends/status?username=
Rooftree::Task<void> GetUserFriendshipStatus(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;
    std::string callerUsername = request->GetAuth().User.Username;

    Reader::UserFriendshipStatusRequest const& in = request->Get<Reader::UserFriendshipStatusRequest>();
    std::string username = in.username() ? in.username()->str() : "";

    if (username.empty())
        co_return request->RespondError(status::bad_request, "username_required");

    if (username == callerUsername)
    {
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(Reader::CreateUserFriendshipStatus(
            builder,
            builder.CreateString("self"),
            flatbuffers::nullopt
        ));
        co_return request->Respond(builder);
    }

    struct StatusResult
    {
        std::string Status = "none";
        Optional<uint32> RequestId;
    };

    Optional<StatusResult> res = co_await session.OnWorker([callerId, username]() -> Optional<StatusResult>
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FOR_FRIEND_BY_NAME);
        stmt->setString(0, username);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
            return {};

        uint32 targetId = row->Fetch()[0].GetUInt32();

        // Check if friends
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIEND_PAIR);
        stmt->setUInt32(0, callerId);
        stmt->setUInt32(1, targetId);
        if (BlogDatabase.Query(stmt))
            return StatusResult{ "friends", {} };

        // Check if pending request exists
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIEND_REQUEST_PAIR);
        stmt->setUInt32(0, callerId);
        stmt->setUInt32(1, targetId);
        stmt->setUInt32(2, targetId);
        stmt->setUInt32(3, callerId);
        if (PreparedQueryResult reqRow = BlogDatabase.Query(stmt))
        {
            Field* fields = reqRow->Fetch();
            uint32 reqId = fields[0].GetUInt32();
            uint32 fromId = fields[1].GetUInt32();
            if (fromId == callerId)
                return StatusResult{ "pending_sent", reqId };
            else
                return StatusResult{ "pending_received", reqId };
        }

        return StatusResult{ "none", {} };
    });
    if (!res)
        co_return request->RespondError(status::not_found, "user_not_found");

    flatbuffers::FlatBufferBuilder builder;
    auto reqId = res->RequestId ? flatbuffers::Optional<uint32>(*res->RequestId) : flatbuffers::nullopt;
    builder.Finish(Reader::CreateUserFriendshipStatus(
        builder,
        builder.CreateString(res->Status),
        reqId
    ));
    request->Respond(builder);
}

} // namespace Blog
