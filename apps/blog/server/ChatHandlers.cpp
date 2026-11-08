/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Real-time chat system: Public square channel, custom user group chats, and mutual friends direct chats.
 * Supports FIFO message trimming, unread cursor tracking, and instant WebSocket push.
 */

#include "Admin_generated.h"
#include "ApiCall.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "Presence.h"
#include "QueryResult.h"
#include "Reader_generated.h"
#include "ServerStatus.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Transaction.h"
#include "Validation.h"
#include <boost/json/serialize.hpp>

namespace Blog
{

using boost::beast::http::status;
using Rooftree::Server::Session;

namespace
{
constexpr uint32 SquareMessageLimit = 5000;
constexpr uint32 DirectMessageLimit = 3000;
const std::string SquareChannel = "square";

struct ParsedDm
{
    uint32 UserA = 0;
    uint32 UserB = 0;
    uint32 OtherUser = 0;
};

Optional<ParsedDm> ParseDmChannel(std::string_view channelId, uint32 callerId)
{
    if (!channelId.starts_with("dm:"))
        return {};

    std::string_view rest = channelId.substr(3);
    std::size_t colon = rest.find(':');
    if (colon == std::string_view::npos)
        return {};

    Optional<uint32> u1 = Rooftree::StringTo<uint32>(rest.substr(0, colon));
    Optional<uint32> u2 = Rooftree::StringTo<uint32>(rest.substr(colon + 1));
    if (!u1 || !u2 || !*u1 || !*u2 || *u1 == *u2)
        return {};
    uint32 minId = std::min(*u1, *u2);
    uint32 maxId = std::max(*u1, *u2);
    if (callerId != minId && callerId != maxId)
        return {};
    return ParsedDm{ minId, maxId, callerId == minId ? maxId : minId };
}

std::string MakeDmChannelId(uint32 u1, uint32 u2)
{
    return Rooftree::StringFormat("dm:{}:{}", std::min(u1, u2), std::max(u1, u2));
}

// One spelling per direct chat, "dm:<lower id>:<higher id>": "dm:9:3" is "dm:3:9", not another chat
std::string NormalizeChannel(std::string channelId)
{
    if (!channelId.starts_with("dm:"))
        return channelId;
    std::string_view rest = std::string_view(channelId).substr(3);
    std::size_t colon = rest.find(':');
    Optional<uint32> u1 = colon == std::string_view::npos ? Optional<uint32>() : Rooftree::StringTo<uint32>(rest.substr(0, colon));
    Optional<uint32> u2 = colon == std::string_view::npos ? Optional<uint32>() : Rooftree::StringTo<uint32>(rest.substr(colon + 1));
    return u1 && u2 ? MakeDmChannelId(*u1, *u2) : channelId;
}

constexpr std::size_t MaxMessageLength = 1000;      // characters, chat_message.content
constexpr std::size_t MaxGroupNameLength = 50;      // chat_group.name
constexpr std::size_t MaxGroupMembers = 100;        // its owner too
constexpr std::size_t MaxChannelIdLength = 64;      // chat_message.channel_id

// The id of "group:<id>"
Optional<uint32> GroupIdOf(std::string_view channelId)
{
    if (!channelId.starts_with("group:"))
        return {};
    Optional<uint32> id = Rooftree::StringTo<uint32>(channelId.substr(6));
    return id && *id ? id : Optional<uint32>();
}

std::string GroupChannel(uint32 groupId)
{
    return "group:" + std::to_string(groupId);
}

bool WorkerIsGroupMember(uint32 groupId, uint32 userId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_GROUP_MEMBER_CHECK);
    stmt->setUInt32(0, groupId);
    stmt->setUInt32(1, userId);
    return BlogDatabase.Query(stmt) != nullptr;
}

std::vector<uint32> WorkerGroupMembers(uint32 groupId)
{
    std::vector<uint32> members;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_GROUP_MEMBER_IDS);
    stmt->setUInt32(0, groupId);
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
            members.push_back(rows->Fetch()[0].GetUInt32());
        while (rows->NextRow());
    }
    return members;
}

bool WorkerAreFriends(uint32 a, uint32 b)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIEND_PAIR);
    stmt->setUInt32(0, a);
    stmt->setUInt32(1, b);
    return BlogDatabase.Query(stmt) != nullptr;
}

uint32 WorkerCountMessages(std::string const& channelId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_UNREAD);
    stmt->setString(0, channelId);
    stmt->setUInt32(1, 0);              // nobody's cursor: all of them
    stmt->setString(2, channelId);
    PreparedQueryResult row = BlogDatabase.Query(stmt);
    return row ? row->Fetch()[0].GetUInt32() : 0;
}

// Whether the caller may read and write a channel, and who its readers are (SendToReaders): the square
// anyone logged in, a group its members, a direct chat its two while friends
struct ChannelAccess
{
    std::string Error;                  // empty: allowed; invalid_channel, not_allowed, not_friends
    std::vector<uint32> Readers;        // none for the square
};

ChannelAccess WorkerChannelAccess(uint32 callerId, std::string const& channelId)
{
    if (channelId == SquareChannel)
        return {};
    if (Optional<uint32> groupId = GroupIdOf(channelId))
    {
        if (!WorkerIsGroupMember(*groupId, callerId))
            return { .Error = "not_allowed" };
        return { .Readers = WorkerGroupMembers(*groupId) };
    }
    Optional<ParsedDm> dm = ParseDmChannel(channelId, callerId);
    if (!dm)
        return { .Error = "invalid_channel" };
    if (!WorkerAreFriends(callerId, dm->OtherUser))
        return { .Error = "not_friends" };
    return { .Readers = { dm->UserA, dm->UserB } };
}

// A channel's messages and read cursors gone
void WorkerClearChannel(std::string const& channelId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_CHANNEL_MESSAGES);
    stmt->setString(0, channelId);
    BlogDatabase.DirectExecute(stmt);
    stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_CHANNEL_READS);
    stmt->setString(0, channelId);
    BlogDatabase.DirectExecute(stmt);
}

// The group, its members (foreign key), messages and read cursors
void WorkerDeleteGroup(uint32 groupId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_GROUP_BY_ID);
    stmt->setUInt32(0, groupId);
    BlogDatabase.DirectExecute(stmt);
    WorkerClearChannel(GroupChannel(groupId));
}

// What the readers of a channel are sent: the square's every logged in page, a group's its members',
// a direct chat's its two's. Never by watching: any page may watch any target (Presence.h)
void SendToReaders(std::string const& channelId, std::vector<uint32> const& readers, std::string_view type, boost::json::object const& body)
{
    if (channelId == SquareChannel)
        return sWorld->GetPresence().SendToLoggedIn(type, body);
    for (uint32 userId : readers)
        sWorld->GetPresence().SendToUser(userId, type, body);
}

// Their channel lists changed: a group made, left, dissolved
void TellChannelsChanged(std::vector<uint32> const& users)
{
    for (uint32 userId : users)
    {
        sWorld->GetPresence().SendToUser(userId, "chat_channels_changed", {});
        sWorld->GetPresence().Changed("user:" + std::to_string(userId), "chat_channels");
    }
}

// Messages removed by a moderator: one (messageId), or the channel's all
void TellRemoved(std::string const& channelId, std::vector<uint32> const& readers, Optional<uint64> messageId)
{
    boost::json::object body = { { "channelId", channelId } };
    if (messageId)
        body["id"] = *messageId;
    SendToReaders(channelId, readers, "chat_removed", body);
    sWorld->GetPresence().Changed("chat:" + channelId, "message");
    for (uint32 userId : readers)
        sWorld->GetPresence().Changed("user:" + std::to_string(userId), "chat_unread");
}

struct MessageRowData
{
    uint64 Id = 0;
    std::string ChannelId;
    uint32 SenderId = 0;
    std::string SenderName;
    std::string SenderAvatar;
    std::string Content;
    SystemTimePoint CreatedAt;
};

MessageRowData FetchMessageRow(Field* fields)
{
    return MessageRowData{
        fields[0].GetUInt64(),
        fields[1].GetString(),
        fields[2].GetUInt32(),
        fields[3].GetString(),
        fields[4].GetStringOrNull().value_or(""),
        fields[5].GetString(),
        fields[6].GetDate()
    };
}

flatbuffers::Offset<Reader::ChatMessageItem> BuildMessageItem(
    flatbuffers::FlatBufferBuilder& builder,
    MessageRowData const& msg)
{
    auto avatar = msg.SenderAvatar.empty() ? 0 : builder.CreateString("/uploads/" + msg.SenderAvatar);
    return Reader::CreateChatMessageItem(
        builder,
        msg.Id,
        builder.CreateString(msg.ChannelId),
        msg.SenderId,
        builder.CreateString(msg.SenderName),
        avatar,
        builder.CreateString(msg.Content),
        TimeString(builder, msg.CreatedAt)
    );
}

} // namespace

// GET /api/chat/channels
Rooftree::Task<void> ListChatChannels(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;

    struct ChannelData
    {
        std::string Id;
        std::string Type;
        std::string Name;
        uint32 FriendId = 0;
        std::string FriendUsername;
        std::string FriendDisplayName;
        std::string FriendAvatar;
        SystemTimePoint FriendSince;
        Optional<MessageRowData> LastMessage;
        uint32 UnreadCount = 0;
        uint32 MemberCount = 0;
    };

    std::vector<ChannelData> channels = co_await session.OnWorker([callerId]()
    {
        std::vector<ChannelData> channels;

        // 1. Square channel
        ChannelData square;
        square.Id = SquareChannel;
        square.Type = "square";
        square.Name = "小屋广场";
        square.MemberCount = 0;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_LAST_MESSAGE);
        stmt->setString(0, SquareChannel);
        if (PreparedQueryResult row = BlogDatabase.Query(stmt))
            square.LastMessage = FetchMessageRow(row->Fetch());

        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_UNREAD);
        stmt->setString(0, SquareChannel);
        stmt->setUInt32(1, callerId);
        stmt->setString(2, SquareChannel);
        if (PreparedQueryResult row = BlogDatabase.Query(stmt))
            square.UnreadCount = row->Fetch()[0].GetUInt32();

        channels.push_back(std::move(square));

        // 2. Custom groups user belongs to
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_CHAT_GROUPS);
        stmt->setUInt32(0, callerId);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                uint32 groupId = fields[0].GetUInt32();
                std::string gId = "group:" + std::to_string(groupId);

                ChannelData grp;
                grp.Id = gId;
                grp.Type = "group";
                grp.Name = fields[1].GetString();
                grp.MemberCount = fields[3].GetUInt32();

                BlogDatabasePreparedStatement* msgStmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_LAST_MESSAGE);
                msgStmt->setString(0, gId);
                if (PreparedQueryResult msgRow = BlogDatabase.Query(msgStmt))
                    grp.LastMessage = FetchMessageRow(msgRow->Fetch());

                BlogDatabasePreparedStatement* unreadStmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_UNREAD);
                unreadStmt->setString(0, gId);
                unreadStmt->setUInt32(1, callerId);
                unreadStmt->setString(2, gId);
                if (PreparedQueryResult unreadRow = BlogDatabase.Query(unreadStmt))
                    grp.UnreadCount = unreadRow->Fetch()[0].GetUInt32();

                channels.push_back(std::move(grp));
            } while (rows->NextRow());
        }

        // 3. Friends direct channels
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIENDS);
        stmt->setUInt32(0, callerId);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                uint32 friendId = fields[0].GetUInt32();
                std::string dmId = MakeDmChannelId(callerId, friendId);

                ChannelData dm;
                dm.Id = dmId;
                dm.Type = "direct";
                dm.Name = fields[2].GetString();
                dm.FriendId = friendId;
                dm.FriendUsername = fields[1].GetString();
                dm.FriendDisplayName = fields[2].GetString();
                dm.FriendAvatar = fields[3].GetStringOrNull().value_or("");
                dm.FriendSince = fields[4].GetDate();
                dm.MemberCount = 2;

                BlogDatabasePreparedStatement* msgStmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_LAST_MESSAGE);
                msgStmt->setString(0, dmId);
                if (PreparedQueryResult msgRow = BlogDatabase.Query(msgStmt))
                    dm.LastMessage = FetchMessageRow(msgRow->Fetch());

                BlogDatabasePreparedStatement* unreadStmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_UNREAD);
                unreadStmt->setString(0, dmId);
                unreadStmt->setUInt32(1, callerId);
                unreadStmt->setString(2, dmId);
                if (PreparedQueryResult unreadRow = BlogDatabase.Query(unreadStmt))
                    dm.UnreadCount = unreadRow->Fetch()[0].GetUInt32();

                channels.push_back(std::move(dm));
            } while (rows->NextRow());
        }

        return channels;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::ChatChannelItem>> items;
    items.reserve(channels.size());

    for (ChannelData const& ch : channels)
    {
        flatbuffers::Offset<Reader::UserFriendItem> friendItem = 0;
        if (ch.Type == "direct")
        {
            bool online = sWorld->GetPresence().FindOnlineUser(ch.FriendId).has_value();
            auto friendAvatar = ch.FriendAvatar.empty() ? 0 : builder.CreateString("/uploads/" + ch.FriendAvatar);
            friendItem = Reader::CreateUserFriendItem(
                builder,
                ch.FriendId,
                builder.CreateString(ch.FriendUsername),
                builder.CreateString(ch.FriendDisplayName),
                friendAvatar,
                online,
                TimeString(builder, ch.FriendSince)
            );
        }

        flatbuffers::Offset<Reader::ChatMessageItem> lastMsg = 0;
        if (ch.LastMessage)
            lastMsg = BuildMessageItem(builder, *ch.LastMessage);

        items.push_back(Reader::CreateChatChannelItem(
            builder,
            builder.CreateString(ch.Id),
            builder.CreateString(ch.Type),
            builder.CreateString(ch.Name),
            0,
            friendItem,
            lastMsg,
            ch.UnreadCount,
            ch.MemberCount
        ));
    }

    builder.Finish(Reader::CreateChatChannelsList(builder, builder.CreateVector(items)));
    request->Respond(builder);
}

// GET /api/chat/channels/{channelId}/messages?before=&limit=
Rooftree::Task<void> ListChatMessages(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;
    Reader::ChatMessagesListRequest const& in = request->Get<Reader::ChatMessagesListRequest>();

    std::string channelId = NormalizeChannel(in.channelId() ? in.channelId()->str() : "");
    if (channelId.empty())
        co_return request->RespondError(status::bad_request, "channel_required");

    uint64 before = in.before().value_or(0);
    uint32 limit = in.limit().value_or(50);
    if (limit == 0 || limit > 50)
        limit = 50;

    Optional<std::pair<std::vector<MessageRowData>, bool>> res = co_await session.OnWorker([callerId, channelId = std::move(channelId), before, limit]() -> Optional<std::pair<std::vector<MessageRowData>, bool>>
    {
        if (!WorkerChannelAccess(callerId, channelId).Error.empty())
            return {};

        std::vector<MessageRowData> messages;
        if (before > 0)
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGES_BEFORE);
            stmt->setString(0, channelId);
            stmt->setUInt64(1, before);
            stmt->setUInt32(2, limit);
            if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
            {
                do
                    messages.push_back(FetchMessageRow(rows->Fetch()));
                while (rows->NextRow());
            }
        }
        else
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGES_INITIAL);
            stmt->setString(0, channelId);
            stmt->setUInt32(1, limit);
            if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
            {
                do
                    messages.push_back(FetchMessageRow(rows->Fetch()));
                while (rows->NextRow());
            }
        }

        bool hasMore = (messages.size() == limit);
        return std::make_pair(std::move(messages), hasMore);
    });
    if (!res)
        co_return request->RespondError(status::forbidden, "not_allowed");

    auto& [messages, hasMore] = *res;
    flatbuffers::FlatBufferBuilder builder;

    // Invert to chronological order (oldest first in list)
    std::vector<flatbuffers::Offset<Reader::ChatMessageItem>> items;
    items.reserve(messages.size());
    for (auto it = messages.rbegin(); it != messages.rend(); ++it)
        items.push_back(BuildMessageItem(builder, *it));

    builder.Finish(Reader::CreateChatMessagesList(builder, builder.CreateVector(items), hasMore));
    request->Respond(builder);
}

// POST /api/chat/channels/{channelId}/messages
Rooftree::Task<void> SendChatMessage(Session& session, ApiRequest request)
{
    AuthInfo const& auth = request->GetAuth();
    uint32 callerId = auth.User.Id;

    if (auth.User.IsDenied(DenyChat))
        co_return request->RespondError(status::forbidden, "not_allowed");

    Reader::ChatMessageSendRequest const& in = request->Get<Reader::ChatMessageSendRequest>();
    TableFields fields(request->GetErrors());
    std::string channelId = NormalizeChannel(fields.Text("channelId", in.channelId(), 1, MaxChannelIdLength));
    std::string content = fields.Text("content", in.content(), 1, MaxMessageLength, true, true);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    struct SendResult
    {
        std::string Error;
        Optional<MessageRowData> Message;
        std::vector<uint32> Readers;
        uint64 TotalMessages = 0;
        uint32 TotalChannels = 0;
    };

    SendResult res = co_await session.OnWorker([callerId, channelId = std::move(channelId), content = std::move(content)]() -> SendResult
    {
        ChannelAccess access = WorkerChannelAccess(callerId, channelId);
        if (!access.Error.empty())
            return { .Error = access.Error };

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_CHAT_MESSAGE);
        stmt->setString(0, channelId);
        stmt->setUInt32(1, callerId);
        stmt->setString(2, content);
        uint64 messageId = BlogDatabase.DirectInsert(stmt);
        if (!messageId)
            return { .Error = "internal_error" };

        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGE);
        stmt->setUInt64(0, messageId);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
            return { .Error = "internal_error" };

        MessageRowData msg = FetchMessageRow(row->Fetch());

        // The sender has read what they sent
        stmt = BlogDatabase.GetPreparedStatement(BLOG_UPS_CHAT_READ);
        stmt->setUInt32(0, callerId);
        stmt->setString(1, channelId);
        stmt->setUInt64(2, msg.Id);
        stmt->setString(3, channelId);
        BlogDatabase.DirectExecute(stmt);

        // Only the newest so many are kept
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_THRESHOLD_FIFO);
        stmt->setString(0, channelId);
        stmt->setUInt32(1, channelId.starts_with("dm:") ? DirectMessageLimit : SquareMessageLimit);
        if (PreparedQueryResult thresholdRow = BlogDatabase.Query(stmt))
        {
            uint64 thresholdId = thresholdRow->Fetch()[0].GetUInt64();
            stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_FIFO);
            stmt->setString(0, channelId);
            stmt->setUInt64(1, thresholdId);
            BlogDatabase.DirectExecute(stmt);
        }

        // For the status page
        SendResult result = { .Message = std::move(msg), .Readers = std::move(access.Readers) };
        if (PreparedQueryResult statsRow = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_STATS)))
        {
            Field* f = statsRow->Fetch();
            result.TotalMessages = f[0].GetUInt64();
            result.TotalChannels = f[1].GetUInt32();
        }
        return result;
    });
    if (res.Error == "internal_error")
        co_return RespondInternalError(request);
    if (!res.Error.empty())
        co_return request->RespondError(res.Error == "invalid_channel" ? status::bad_request : status::forbidden, res.Error);

    MessageRowData const& msg = *res.Message;
    sWorld->GetServerStatus().SetChatStats(res.TotalMessages, res.TotalChannels);

    // The message itself to its readers' pages at once (the sender's other pages too)
    boost::json::object msgJson = {
        { "id", msg.Id },
        { "channelId", msg.ChannelId },
        { "senderId", msg.SenderId },
        { "senderName", msg.SenderName },
        { "senderAvatar", msg.SenderAvatar.empty() ? boost::json::value(nullptr) : boost::json::value("/uploads/" + msg.SenderAvatar) },
        { "content", msg.Content },
        { "createdAt", FormatTime(msg.CreatedAt) }
    };
    SendToReaders(msg.ChannelId, res.Readers, "chat_message", msgJson);
    sWorld->GetPresence().Changed("chat:" + msg.ChannelId, "message");
    for (uint32 userId : res.Readers)
        if (userId != callerId)
            sWorld->GetPresence().Changed("user:" + std::to_string(userId), "chat_unread");

    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(BuildMessageItem(builder, msg));
    request->Respond(builder);
}

// POST /api/chat/channels/{channelId}/read
Rooftree::Task<void> MarkChatRead(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;
    Reader::ChatMarkReadRequest const& in = request->Get<Reader::ChatMarkReadRequest>();

    std::string channelId = NormalizeChannel(in.channelId() ? in.channelId()->str() : "");
    uint64 lastReadId = in.lastReadId();

    if (channelId.empty() || channelId.size() > MaxChannelIdLength || lastReadId == 0)
        co_return RespondOk(request);

    // Only a channel the caller may read: no read cursors for others' chats. The cursor never passes
    // the channel's newest message, so a bogus id can't hide the messages still to come
    bool marked = co_await session.OnWorker([callerId, channelId = std::move(channelId), lastReadId]()
    {
        if (!WorkerChannelAccess(callerId, channelId).Error.empty())
            return false;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPS_CHAT_READ);
        stmt->setUInt32(0, callerId);
        stmt->setString(1, channelId);
        stmt->setUInt64(2, lastReadId);
        stmt->setString(3, channelId);
        BlogDatabase.DirectExecute(stmt);
        return true;
    });
    if (marked)
        sWorld->GetPresence().Changed("user:" + std::to_string(callerId), "chat_unread");
    RespondOk(request);
}

// GET /api/chat/unread
Rooftree::Task<void> GetChatUnreadCount(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;

    uint32 count = co_await session.OnWorker([callerId]()
    {
        uint32 total = 0;

        // Square unread
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_UNREAD);
        stmt->setString(0, SquareChannel);
        stmt->setUInt32(1, callerId);
        stmt->setString(2, SquareChannel);
        if (PreparedQueryResult row = BlogDatabase.Query(stmt))
            total += row->Fetch()[0].GetUInt32();

        // Custom groups unread
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_CHAT_GROUPS);
        stmt->setUInt32(0, callerId);
        if (PreparedQueryResult gRows = BlogDatabase.Query(stmt))
        {
            do
            {
                uint32 gId = gRows->Fetch()[0].GetUInt32();
                std::string chanId = "group:" + std::to_string(gId);
                BlogDatabasePreparedStatement* uStmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_UNREAD);
                uStmt->setString(0, chanId);
                uStmt->setUInt32(1, callerId);
                uStmt->setString(2, chanId);
                if (PreparedQueryResult uRow = BlogDatabase.Query(uStmt))
                    total += uRow->Fetch()[0].GetUInt32();
            } while (gRows->NextRow());
        }

        // Direct friends chats unread
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIENDS);
        stmt->setUInt32(0, callerId);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                uint32 friendId = rows->Fetch()[0].GetUInt32();
                std::string dmId = MakeDmChannelId(callerId, friendId);
                BlogDatabasePreparedStatement* uStmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_UNREAD);
                uStmt->setString(0, dmId);
                uStmt->setUInt32(1, callerId);
                uStmt->setString(2, dmId);
                if (PreparedQueryResult uRow = BlogDatabase.Query(uStmt))
                    total += uRow->Fetch()[0].GetUInt32();
            } while (rows->NextRow());
        }

        return total;
    });
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateChatUnreadCount(builder, count));
    request->Respond(builder);
}

// POST /api/chat/groups
Rooftree::Task<void> CreateChatGroup(Session& session, ApiRequest request)
{
    AuthInfo const& auth = request->GetAuth();
    uint32 callerId = auth.User.Id;

    if (auth.User.IsDenied(DenyChat))
        co_return request->RespondError(status::forbidden, "not_allowed");

    Reader::ChatGroupCreateRequest const& in = request->Get<Reader::ChatGroupCreateRequest>();
    TableFields fields(request->GetErrors());
    std::string name = fields.Text("name", in.name(), 1, MaxGroupNameLength);

    std::vector<uint32> memberIds;
    if (in.memberIds())
        for (uint32 id : *in.memberIds())
            if (id && id != callerId && std::ranges::find(memberIds, id) == memberIds.end())
                memberIds.push_back(id);
    if (memberIds.size() + 1 > MaxGroupMembers)
        fields.AddError("memberIds", "too_long");
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    struct GroupResult
    {
        std::string Error;
        uint32 GroupId = 0;
        std::vector<uint32> Members;
    };

    GroupResult res = co_await session.OnWorker([callerId, name, memberIds = std::move(memberIds)]() -> GroupResult
    {
        // Only friends can be put in
        for (uint32 memberId : memberIds)
            if (!WorkerAreFriends(callerId, memberId))
                return { .Error = "not_friends" };

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_CHAT_GROUP);
        stmt->setString(0, name);
        stmt->setUInt32(1, callerId);
        uint32 groupId = uint32(BlogDatabase.DirectInsert(stmt));
        if (!groupId)
            return { .Error = "internal_error" };

        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
        std::vector<uint32> members = memberIds;
        members.push_back(callerId);
        for (uint32 memberId : members)
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_CHAT_GROUP_MEMBER);
            stmt->setUInt32(0, groupId);
            stmt->setUInt32(1, memberId);
            stmt->setString(2, std::string(memberId == callerId ? "owner" : "member"));
            trans->Append(stmt);
        }
        BlogDatabase.DirectCommitTransaction(trans);
        return { .GroupId = groupId, .Members = std::move(members) };
    });
    if (res.Error == "internal_error")
        co_return RespondInternalError(request);
    if (!res.Error.empty())
        co_return request->RespondError(status::forbidden, res.Error);

    TellChannelsChanged(res.Members);

    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateChatChannelItem(
        builder,
        builder.CreateString(GroupChannel(res.GroupId)),
        builder.CreateString("group"),
        builder.CreateString(name),
        0,
        0,
        0,
        0,
        uint32(res.Members.size())
    ));
    request->Respond(builder);
}

// GET /api/chat/groups/{id}/members
Rooftree::Task<void> ListChatGroupMembers(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;
    uint32 groupId = request->Get<Reader::ChatGroupActionRequest>().id();

    struct MemberData
    {
        uint32 Id = 0;
        std::string Username;
        std::string DisplayName;
        std::string Avatar;
        std::string Role;
    };

    Optional<std::vector<MemberData>> members = co_await session.OnWorker([callerId, groupId]() -> Optional<std::vector<MemberData>>
    {
        // Check caller is member
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_GROUP_MEMBER_CHECK);
        stmt->setUInt32(0, groupId);
        stmt->setUInt32(1, callerId);
        if (!BlogDatabase.Query(stmt))
            return {};

        std::vector<MemberData> members;
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_GROUP_MEMBERS);
        stmt->setUInt32(0, groupId);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* f = rows->Fetch();
                members.push_back({
                    f[0].GetUInt32(),
                    f[1].GetString(),
                    f[2].GetString(),
                    f[3].GetStringOrNull().value_or(""),
                    f[4].GetString()
                });
            } while (rows->NextRow());
        }
        return members;
    });
    if (!members)
        co_return request->RespondError(status::forbidden, "not_allowed");

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::ChatGroupMemberItem>> items;
    items.reserve(members->size());

    for (MemberData const& m : *members)
    {
        bool online = sWorld->GetPresence().FindOnlineUser(m.Id).has_value();
        auto avatar = m.Avatar.empty() ? 0 : builder.CreateString("/uploads/" + m.Avatar);
        items.push_back(Reader::CreateChatGroupMemberItem(
            builder,
            m.Id,
            builder.CreateString(m.Username),
            builder.CreateString(m.DisplayName),
            avatar,
            builder.CreateString(m.Role),
            online
        ));
    }

    builder.Finish(Reader::CreateChatGroupMembersList(builder, builder.CreateVector(items)));
    request->Respond(builder);
}

// POST /api/chat/groups/{id}/leave
// A member leaves; the owner leaving dissolves the group, its messages with it
Rooftree::Task<void> LeaveChatGroup(Session& session, ApiRequest request)
{
    uint32 callerId = request->GetAuth().User.Id;
    uint32 groupId = request->Get<Reader::ChatGroupActionRequest>().id();

    struct LeaveResult
    {
        bool Found = false;
        bool Dissolved = false;
        std::vector<uint32> Members;    // before leaving: the caller among them
    };

    LeaveResult res = co_await session.OnWorker([callerId, groupId]() -> LeaveResult
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_GROUP_MEMBER_CHECK);
        stmt->setUInt32(0, groupId);
        stmt->setUInt32(1, callerId);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
            return {};

        LeaveResult result = { .Found = true, .Dissolved = row->Fetch()[0].GetString() == "owner", .Members = WorkerGroupMembers(groupId) };
        if (result.Dissolved)
            WorkerDeleteGroup(groupId);
        else
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_GROUP_MEMBER);
            stmt->setUInt32(0, groupId);
            stmt->setUInt32(1, callerId);
            BlogDatabase.DirectExecute(stmt);
        }
        return result;
    });
    if (!res.Found)
        co_return request->RespondError(status::not_found, "group_not_found");

    // Gone from everyone's list, or the member count changed
    TellChannelsChanged(res.Members);
    if (res.Dissolved)
        TellRemoved(GroupChannel(groupId), res.Members, {});
    RespondOk(request);
}

// ---- moderation (Admin.fbs): the square and the groups, never the direct chats

namespace
{
// A channel a moderator may look into: the square or a group's ("group:<id>")
bool IsModeratedChannel(std::string_view channelId)
{
    return channelId == SquareChannel || GroupIdOf(channelId).has_value();
}

// Who reads it, for telling them what went: the square's are all logged in pages (SendToReaders)
std::vector<uint32> WorkerReadersOf(std::string const& channelId)
{
    Optional<uint32> groupId = GroupIdOf(channelId);
    return groupId ? WorkerGroupMembers(*groupId) : std::vector<uint32>();
}

void RespondRemoved(ApiRequest const& request, uint32 removed)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Admin::CreateChatRemoved(builder, removed));
    request->Respond(builder);
}
}

// GET /api/admin/chat/groups
Rooftree::Task<void> AdminListChatGroups(Session& session, ApiRequest request)
{
    struct GroupRow
    {
        uint32 Id = 0;
        std::string Name;
        uint32 OwnerId = 0;
        std::string OwnerUsername;
        uint32 Members = 0;
        uint32 Messages = 0;
        SystemTimePoint CreatedAt;
    };

    std::vector<GroupRow> groups = co_await session.OnWorker([]
    {
        std::vector<GroupRow> groups;
        if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_CHAT_GROUPS)))
        {
            do
            {
                Field* f = rows->Fetch();
                groups.push_back({ f[0].GetUInt32(), f[1].GetString(), f[2].GetUInt32(), f[3].GetString(), uint32(f[4].GetUInt64()),
                    uint32(f[5].GetUInt64()), f[6].GetDate() });
            } while (rows->NextRow());
        }
        return groups;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::ChatGroup>> items;
    for (GroupRow const& g : groups)
        items.push_back(Admin::CreateChatGroup(builder, g.Id, builder.CreateString(g.Name), g.OwnerId, builder.CreateString(g.OwnerUsername),
            g.Members, g.Messages, TimeString(builder, g.CreatedAt)));
    builder.Finish(Admin::CreateChatGroupsList(builder, builder.CreateVector(items)));
    request->Respond(builder);
}

// GET /api/admin/chat/messages?channel=&cursor=, newest first, 50 a page
Rooftree::Task<void> AdminListChatMessages(Session& session, ApiRequest request)
{
    Admin::ChatMessagesListRequest const& in = request->Get<Admin::ChatMessagesListRequest>();
    TableFields fields(request->GetErrors());
    std::string channelId = fields.Text("channel", in.channel(), 1, MaxChannelIdLength);
    if (!channelId.empty() && !IsModeratedChannel(channelId))
        fields.AddError("channel", "invalid");
    uint64 before = (in.cursor() ? Rooftree::StringTo<uint64>(in.cursor()->string_view()) : Optional<uint64>()).value_or(std::numeric_limits<uint64>::max());
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    struct Row
    {
        uint64 Id = 0;
        std::string ChannelId;
        uint32 SenderId = 0;
        std::string SenderUsername;
        std::string SenderName;
        std::string Content;
        SystemTimePoint CreatedAt;
    };

    std::vector<Row> rows = co_await session.OnWorker([channelId, before]
    {
        std::vector<Row> rows;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_CHAT_MESSAGES);
        stmt->setString(0, channelId);
        stmt->setUInt64(1, before);
        if (PreparedQueryResult result = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* f = result->Fetch();
                rows.push_back({ f[0].GetUInt64(), f[1].GetString(), f[2].GetUInt32(), f[3].GetString(), f[4].GetString(), f[5].GetString(), f[6].GetDate() });
            } while (result->NextRow());
        }
        return rows;
    });
    // The 51st only says there is more
    bool more = rows.size() > 50;
    if (more)
        rows.resize(50);

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::ChatMessage>> items;
    for (Row const& m : rows)
        items.push_back(Admin::CreateChatMessage(builder, m.Id, builder.CreateString(m.ChannelId), m.SenderId, builder.CreateString(m.SenderUsername),
            builder.CreateString(m.SenderName), builder.CreateString(m.Content), TimeString(builder, m.CreatedAt)));
    auto cursor = more ? builder.CreateString(std::to_string(rows.back().Id)) : 0;
    builder.Finish(Admin::CreateChatMessagesList(builder, builder.CreateVector(items), cursor));
    request->Respond(builder);
}

// DELETE /api/admin/chat/messages/{id}
Rooftree::Task<void> AdminDeleteChatMessage(Session& session, ApiRequest request)
{
    uint64 messageId = request->Get<Admin::ChatMessageRequest>().id().value_or(0);

    struct Deleted
    {
        std::string ChannelId;          // empty: no such message the moderators may see
        std::vector<uint32> Readers;
    };

    Deleted deleted = co_await session.OnWorker([messageId]() -> Deleted
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGE);
        stmt->setUInt64(0, messageId);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row || !IsModeratedChannel(row->Fetch()[1].GetStringView()))
            return {};

        Deleted deleted = { .ChannelId = row->Fetch()[1].GetString() };
        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_MESSAGE);
        stmt->setUInt64(0, messageId);
        BlogDatabase.DirectExecute(stmt);
        deleted.Readers = WorkerReadersOf(deleted.ChannelId);
        return deleted;
    });
    if (deleted.ChannelId.empty())
        co_return request->RespondError(status::not_found, "not_found");

    RT_LOG_INFO("blog.admin", "{} deleted the chat message {} of {}", request->GetAuth().User.Username, messageId, deleted.ChannelId);
    TellRemoved(deleted.ChannelId, deleted.Readers, messageId);
    RespondRemoved(request, 1);
}

// DELETE /api/admin/chat/channels/{channel}/messages: the square's or a group's every message
Rooftree::Task<void> AdminClearChatChannel(Session& session, ApiRequest request)
{
    flatbuffers::String const* value = request->Get<Admin::ChatChannelRequest>().channel();
    std::string channelId = value ? value->str() : std::string();
    if (!IsModeratedChannel(channelId))
        co_return request->RespondError(status::not_found, "not_found");

    struct Cleared
    {
        bool Found = false;
        uint32 Removed = 0;
        std::vector<uint32> Readers;
    };

    Cleared cleared = co_await session.OnWorker([channelId]() -> Cleared
    {
        Optional<uint32> groupId = GroupIdOf(channelId);
        std::vector<uint32> readers = WorkerReadersOf(channelId);
        if (groupId && readers.empty())
            return {};
        uint32 removed = WorkerCountMessages(channelId);
        WorkerClearChannel(channelId);
        return { true, removed, std::move(readers) };
    });
    if (!cleared.Found)
        co_return request->RespondError(status::not_found, "not_found");

    RT_LOG_INFO("blog.admin", "{} cleared {}: {} messages", request->GetAuth().User.Username, channelId, cleared.Removed);
    TellRemoved(channelId, cleared.Readers, {});
    RespondRemoved(request, cleared.Removed);
}

// DELETE /api/admin/chat/groups/{id}: dissolved, its messages with it
Rooftree::Task<void> AdminDeleteChatGroup(Session& session, ApiRequest request)
{
    uint32 groupId = request->Get<Admin::IdRequest>().id().value_or(0);

    struct Dissolved
    {
        uint32 Removed = 0;
        std::vector<uint32> Members;    // none: no such group
    };

    Dissolved dissolved = co_await session.OnWorker([groupId]() -> Dissolved
    {
        std::vector<uint32> members = WorkerGroupMembers(groupId);
        if (members.empty())
            return {};
        uint32 removed = WorkerCountMessages(GroupChannel(groupId));
        WorkerDeleteGroup(groupId);
        return { removed, std::move(members) };
    });
    if (dissolved.Members.empty())
        co_return request->RespondError(status::not_found, "not_found");

    RT_LOG_INFO("blog.admin", "{} dissolved the chat group {} ({} messages)", request->GetAuth().User.Username, groupId, dissolved.Removed);
    TellChannelsChanged(dissolved.Members);
    TellRemoved(GroupChannel(groupId), dissolved.Members, {});
    RespondRemoved(request, dissolved.Removed);
}

} // namespace Blog
