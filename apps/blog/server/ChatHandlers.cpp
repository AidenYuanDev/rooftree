/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Real-time chat: the public square, readers' groups and direct chats between friends. Messages may
 * quote another of the same chat, carry a file (a picture is shown), @mention accounts, be edited
 * or taken back by their sender for a while. Each chat keeps its newest so many (FIFO), every
 * account its own read cursor, pinned and muted chats. New messages and changes reach the readers'
 * pages at once over the WebSocket, never by watching (Presence.h).
 *
 * Files: Chat.h. Moderation (the square and the groups, never the direct chats) at the end.
 */

#include "Accounts.h"
#include "Admin_generated.h"
#include "ApiCall.h"
#include "Avatar.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Chat.h"
#include "Drive.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "Presence.h"
#include "QueryResult.h"
#include "Reader_generated.h"
#include "SecurityUtils.h"
#include "ServerStatus.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Transaction.h"
#include "Util.h"
#include "Validation.h"

#include <boost/json/array.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>

namespace Blog {

namespace http = boost::beast::http;
using boost::beast::http::status;
using Rooftree::Server::Session;

namespace {
constexpr uint32 SquareMessageLimit = 5000;
constexpr uint32 DirectMessageLimit = 3000;
const std::string SquareChannel = "square";

constexpr std::size_t MaxMessageLength = 1000;  // characters, chat_message.content
constexpr std::size_t MaxGroupNameLength = 50;  // chat_group.name
constexpr std::size_t MaxGroupMembers = 100;    // its owner too
constexpr std::size_t MaxChannelIdLength = 64;  // chat_message.channel_id
constexpr std::size_t MaxFileNameLength = 255;  // chat_message.file_name
constexpr std::size_t MaxMentions = 10;         // accounts one message @mentions
constexpr std::size_t MaxSearchLength = 100;
constexpr uint32 SearchPageSize = 50;

struct ParsedDm {
  uint32 UserA = 0;
  uint32 UserB = 0;
  uint32 OtherUser = 0;
};

Optional<ParsedDm> ParseDmChannel(std::string_view channelId, uint32 callerId) {
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
  return ParsedDm{minId, maxId, callerId == minId ? maxId : minId};
}

std::string MakeDmChannelId(uint32 u1, uint32 u2) {
  return Rooftree::StringFormat("dm:{}:{}", std::min(u1, u2), std::max(u1, u2));
}

// One spelling per direct chat, "dm:<lower id>:<higher id>": "dm:9:3" is "dm:3:9", not another chat
std::string NormalizeChannel(std::string channelId) {
  if (!channelId.starts_with("dm:"))
    return channelId;
  std::string_view rest = std::string_view(channelId).substr(3);
  std::size_t colon = rest.find(':');
  Optional<uint32> u1 = colon == std::string_view::npos
                            ? Optional<uint32>()
                            : Rooftree::StringTo<uint32>(rest.substr(0, colon));
  Optional<uint32> u2 = colon == std::string_view::npos
                            ? Optional<uint32>()
                            : Rooftree::StringTo<uint32>(rest.substr(colon + 1));
  return u1 && u2 ? MakeDmChannelId(*u1, *u2) : channelId;
}

// The id of "group:<id>"
Optional<uint32> GroupIdOf(std::string_view channelId) {
  if (!channelId.starts_with("group:"))
    return {};
  Optional<uint32> id = Rooftree::StringTo<uint32>(channelId.substr(6));
  return id && *id ? id : Optional<uint32>();
}

std::string GroupChannel(uint32 groupId) {
  return "group:" + std::to_string(groupId);
}

// The role ("owner", "member") of a member, none for anyone else
Optional<std::string> WorkerGroupRole(uint32 groupId, uint32 userId) {
  BlogDatabasePreparedStatement* stmt =
      BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_GROUP_MEMBER_CHECK);
  stmt->setUInt32(0, groupId);
  stmt->setUInt32(1, userId);
  PreparedQueryResult row = BlogDatabase.Query(stmt);
  return row ? Optional<std::string>(row->Fetch()[0].GetString()) : Optional<std::string>();
}

bool WorkerIsGroupMember(uint32 groupId, uint32 userId) {
  return WorkerGroupRole(groupId, userId).has_value();
}

std::vector<uint32> WorkerGroupMembers(uint32 groupId) {
  std::vector<uint32> members;
  BlogDatabasePreparedStatement* stmt =
      BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_GROUP_MEMBER_IDS);
  stmt->setUInt32(0, groupId);
  if (PreparedQueryResult rows = BlogDatabase.Query(stmt)) {
    do {
      members.push_back(rows->Fetch()[0].GetUInt32());
    } while (rows->NextRow());
  }
  return members;
}

bool WorkerAreFriends(uint32 a, uint32 b) {
  BlogDatabasePreparedStatement* stmt =
      BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIEND_PAIR);
  stmt->setUInt32(0, a);
  stmt->setUInt32(1, b);
  return BlogDatabase.Query(stmt) != nullptr;
}

uint32 WorkerCountMessages(std::string const& channelId) {
  BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_UNREAD);
  stmt->setString(0, channelId);
  stmt->setUInt32(1, 0);  // nobody's cursor: all of them
  stmt->setString(2, channelId);
  PreparedQueryResult row = BlogDatabase.Query(stmt);
  return row ? row->Fetch()[0].GetUInt32() : 0;
}

// Whether the caller may read and write a channel, and who its readers are (SendToReaders): the
// square anyone logged in, a group its members, a direct chat its two while friends
struct ChannelAccess {
  std::string Error;            // empty: allowed; invalid_channel, not_allowed, not_friends
  std::vector<uint32> Readers;  // none for the square
};

ChannelAccess WorkerChannelAccess(uint32 callerId, std::string const& channelId) {
  if (channelId == SquareChannel)
    return {};
  if (Optional<uint32> groupId = GroupIdOf(channelId)) {
    if (!WorkerIsGroupMember(*groupId, callerId))
      return {.Error = "not_allowed"};
    return {.Readers = WorkerGroupMembers(*groupId)};
  }
  Optional<ParsedDm> dm = ParseDmChannel(channelId, callerId);
  if (!dm)
    return {.Error = "invalid_channel"};
  if (!WorkerAreFriends(callerId, dm->OtherUser))
    return {.Error = "not_friends"};
  return {.Readers = {dm->UserA, dm->UserB}};
}

// An access error as the answer says it
void RespondAccessError(ApiRequest const& request, std::string const& error) {
  request->RespondError(
      error == "invalid_channel" ? status::bad_request : status::forbidden, error);
}

// ---- files

void RemoveChatFile(std::string const& directory, std::string_view stored) {
  if (stored.empty() || stored.find('/') != std::string_view::npos ||
      stored.find("..") != std::string_view::npos)
    return;
  std::error_code error;
  std::filesystem::remove(directory + "/" + std::string(stored), error);
}

// The files a statement selects (file_stored first), from the chat's directory
void WorkerRemoveSelectedFiles(BlogDatabasePreparedStatement* stmt, std::string const& directory) {
  if (PreparedQueryResult rows = BlogDatabase.Query(stmt)) {
    do {
      RemoveChatFile(directory, rows->Fetch()[0].GetStringView());
    } while (rows->NextRow());
  }
}

// A channel's messages (their files first), read cursors and mentions gone
void WorkerClearChannel(std::string const& channelId, std::string const& directory) {
  BlogDatabasePreparedStatement* stmt =
      BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_CHANNEL_FILES);
  stmt->setString(0, channelId);
  WorkerRemoveSelectedFiles(stmt, directory);

  stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_CHANNEL_MESSAGES);
  stmt->setString(0, channelId);
  BlogDatabase.DirectExecute(stmt);
  stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_CHANNEL_READS);
  stmt->setString(0, channelId);
  BlogDatabase.DirectExecute(stmt);
}

// The group, its members (foreign key), messages, read cursors and its members' settings of it
void WorkerDeleteGroup(uint32 groupId, std::string const& directory) {
  BlogDatabasePreparedStatement* stmt =
      BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_GROUP_BY_ID);
  stmt->setUInt32(0, groupId);
  BlogDatabase.DirectExecute(stmt);
  WorkerClearChannel(GroupChannel(groupId), directory);
  stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_CHANNEL_PREFS);
  stmt->setString(0, GroupChannel(groupId));
  BlogDatabase.DirectExecute(stmt);
}

// ---- pushes

// What the readers of a channel are sent: the square's every logged in page, a group's its
// members', a direct chat's its two's. Never by watching: any page
// may watch any target (Presence.h)
void SendToReaders(
    std::string const& channelId,
    std::vector<uint32> const& readers,
    std::string_view type,
    boost::json::object const& body) {
  if (channelId == SquareChannel)
    return sWorld->GetPresence().SendToLoggedIn(type, body);
  for (uint32 userId : readers)
    sWorld->GetPresence().SendToUser(userId, type, body);
}

// Their channel lists changed: a group made, renamed, left, dissolved, members in or out
void TellChannelsChanged(std::vector<uint32> const& users) {
  for (uint32 userId : users) {
    sWorld->GetPresence().SendToUser(userId, "chat_channels_changed", {});
    sWorld->GetPresence().Changed("user:" + std::to_string(userId), "chat_channels");
  }
}

// Messages removed by a moderator: one (messageId), or the channel's all
void TellRemoved(
    std::string const& channelId,
    std::vector<uint32> const& readers,
    Optional<uint64> messageId) {
  boost::json::object body = {{"channelId", channelId}};
  if (messageId)
    body["id"] = *messageId;
  SendToReaders(channelId, readers, "chat_removed", body);
  sWorld->GetPresence().Changed("chat:" + channelId, "message");
  for (uint32 userId : readers)
    sWorld->GetPresence().Changed("user:" + std::to_string(userId), "chat_unread");
}

// ---- messages

struct ReplyData {
  uint64 Id = 0;
  uint32 SenderId = 0;
  std::string SenderName;
  std::string Content;
  bool HasFile = false;
  bool Recalled = false;
};

struct FileData {
  std::string Name;
  std::string Mime;
  uint64 Size = 0;
};

struct MessageRowData {
  uint64 Id = 0;
  std::string ChannelId;
  uint32 SenderId = 0;
  std::string SenderName;
  std::string SenderAvatar;
  std::string Content;
  SystemTimePoint CreatedAt;
  Optional<SystemTimePoint> EditedAt;
  bool Recalled = false;
  Optional<FileData> File;
  Optional<ReplyData> Reply;
  std::vector<uint32> Mentions;
};

// BLOG_CHAT_MESSAGE_SELECT's columns
MessageRowData FetchMessageRow(Field* fields) {
  MessageRowData msg{
      .Id = fields[0].GetUInt64(),
      .ChannelId = fields[1].GetString(),
      .SenderId = fields[2].GetUInt32(),
      .SenderName = fields[3].GetString(),
      .SenderAvatar = fields[4].GetStringOrNull().value_or(""),
      .Content = fields[5].GetString(),
      .CreatedAt = fields[6].GetDate(),
      .EditedAt = fields[7].GetDateOrNull(),
      .Recalled = fields[8].GetBool(),
  };
  if (!fields[9].IsNull())
    msg.File = FileData{fields[9].GetString(), fields[10].GetString(), fields[11].GetUInt64()};
  if (!fields[12].IsNull() && !fields[13].IsNull()) {
    msg.Reply = ReplyData{fields[12].GetUInt64(), fields[13].GetUInt32(), fields[14].GetString(),
                          fields[15].GetString(), fields[17].GetBool(),   fields[16].GetBool()};
  }
  for (std::string_view id : Rooftree::Tokenize(fields[18].GetStringView(), ',', false)) {
    if (Optional<uint32> value = Rooftree::StringTo<uint32>(id))
      msg.Mentions.push_back(*value);
  }
  return msg;
}

Optional<MessageRowData> WorkerMessage(uint64 messageId) {
  BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGE);
  stmt->setUInt64(0, messageId);
  PreparedQueryResult row = BlogDatabase.Query(stmt);
  return row ? Optional<MessageRowData>(FetchMessageRow(row->Fetch())) : Optional<MessageRowData>();
}

std::vector<MessageRowData> WorkerMessages(BlogDatabasePreparedStatement* stmt) {
  std::vector<MessageRowData> messages;
  if (PreparedQueryResult rows = BlogDatabase.Query(stmt)) {
    do {
      messages.push_back(FetchMessageRow(rows->Fetch()));
    } while (rows->NextRow());
  }
  return messages;
}

bool IsChatImage(std::string_view mime) {
  return mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" || mime == "image/webp";
}

std::string FileUrl(uint64 messageId) {
  return "/api/chat/files/" + std::to_string(messageId);
}

std::string AvatarUrl(std::string const& avatar) {
  return avatar.empty() ? std::string() : "/uploads/" + avatar;
}

flatbuffers::Offset<Reader::ChatMessageItem> BuildMessageItem(
    flatbuffers::FlatBufferBuilder& builder,
    MessageRowData const& msg) {
  flatbuffers::Offset<Reader::ChatReplyItem> reply = 0;
  if (msg.Reply) {
    reply = Reader::CreateChatReplyItem(
        builder, msg.Reply->Id, msg.Reply->SenderId, builder.CreateString(msg.Reply->SenderName),
        builder.CreateString(msg.Reply->Recalled ? std::string() : msg.Reply->Content),
        msg.Reply->HasFile, msg.Reply->Recalled);
  }
  flatbuffers::Offset<Reader::ChatFileItem> file = 0;
  if (msg.File) {
    file = Reader::CreateChatFileItem(
        builder, builder.CreateString(msg.File->Name), builder.CreateString(msg.File->Mime),
        msg.File->Size, IsChatImage(msg.File->Mime), builder.CreateString(FileUrl(msg.Id)));
  }
  auto avatar = msg.SenderAvatar.empty() ? 0 : builder.CreateString(AvatarUrl(msg.SenderAvatar));
  auto channel = builder.CreateString(msg.ChannelId);
  auto name = builder.CreateString(msg.SenderName);
  auto content = builder.CreateString(msg.Content);
  auto created = TimeString(builder, msg.CreatedAt);
  auto edited = msg.EditedAt ? TimeString(builder, *msg.EditedAt) : 0;
  auto mentions = builder.CreateVector(msg.Mentions);
  return Reader::CreateChatMessageItem(
      builder,
      msg.Id,
      channel,
      msg.SenderId,
      name,
      avatar,
      content,
      created,
      reply,
      file,
      edited,
      msg.Recalled,
      mentions);
}

// The same as JSON, for the WebSocket: what the API answers for a ChatMessageItem
boost::json::object MessageJson(MessageRowData const& msg) {
  boost::json::value reply = nullptr;
  if (msg.Reply) {
    reply = boost::json::object{
        {"id", msg.Reply->Id},
        {"senderId", msg.Reply->SenderId},
        {"senderName", msg.Reply->SenderName},
        {"content", msg.Reply->Recalled ? std::string() : msg.Reply->Content},
        {"hasFile", msg.Reply->HasFile},
        {"recalled", msg.Reply->Recalled}};
  }
  boost::json::value file = nullptr;
  if (msg.File) {
    file = boost::json::object{
        {"name", msg.File->Name},
        {"mime", msg.File->Mime},
        {"size", msg.File->Size},
        {"image", IsChatImage(msg.File->Mime)},
        {"url", FileUrl(msg.Id)}};
  }
  boost::json::array mentions;
  for (uint32 id : msg.Mentions)
    mentions.push_back(id);
  return {
      {"id", msg.Id},
      {"channelId", msg.ChannelId},
      {"senderId", msg.SenderId},
      {"senderName", msg.SenderName},
      {"senderAvatar", msg.SenderAvatar.empty() ? boost::json::value(nullptr)
                                                : boost::json::value(AvatarUrl(msg.SenderAvatar))},
      {"content", msg.Content},
      {"createdAt", FormatTime(msg.CreatedAt)},
      {"replyTo", std::move(reply)},
      {"file", std::move(file)},
      {"editedAt",
       msg.EditedAt ? boost::json::value(FormatTime(*msg.EditedAt)) : boost::json::value(nullptr)},
      {"recalled", msg.Recalled},
      {"mentions", std::move(mentions)},
  };
}

// The usernames a text @mentions: "@name" not inside a word, at most MaxMentions different ones
std::vector<std::string> MentionedNames(std::string_view text) {
  auto isNameChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  std::vector<std::string> names;
  for (std::size_t i = 0; i < text.size() && names.size() < MaxMentions; ++i) {
    if (text[i] != '@' || (i > 0 && isNameChar(text[i - 1])))
      continue;
    std::size_t end = i + 1;
    while (end < text.size() && isNameChar(text[end]))
      ++end;
    std::string name(text.substr(i + 1, end - i - 1));
    std::ranges::transform(name, name.begin(), [](unsigned char c) {
      return char(std::tolower(c));
    });
    if (IsValidUsername(name) && std::ranges::find(names, name) == names.end())
      names.push_back(std::move(name));
    i = end - 1;
  }
  return names;
}

// Worker: the accounts names stand for that may read the channel, the sender left out
std::vector<uint32> WorkerResolveMentions(
    std::vector<std::string> const& names,
    std::string const& channelId,
    std::vector<uint32> const& readers,
    uint32 senderId) {
  std::vector<uint32> users;
  for (std::string const& name : names) {
    BlogDatabasePreparedStatement* stmt =
        BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MENTION_USER);
    stmt->setString(0, name);
    PreparedQueryResult row = BlogDatabase.Query(stmt);
    if (!row)
      continue;
    uint32 userId = row->Fetch()[0].GetUInt32();
    bool reads = channelId == SquareChannel || std::ranges::find(readers, userId) != readers.end();
    if (userId != senderId && reads && std::ranges::find(users, userId) == users.end())
      users.push_back(userId);
  }
  return users;
}

// The first characters of a text, "…" when there was more
std::string Excerpt(std::string_view text, std::size_t characters) {
  std::string excerpt;
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size() && count < characters; ++count) {
    std::size_t length = 1;
    unsigned char c = text[i];
    if (c >= 0xf0) {
      length = 4;
    } else if (c >= 0xe0) {
      length = 3;
    } else if (c >= 0xc0) {
      length = 2;
    }
    excerpt.append(text.substr(i, length));
    i += length;
  }
  if (excerpt.size() < text.size())
    excerpt += "…";
  return excerpt;
}

// To someone @mentioned while away: who, where, what they wrote, a link, how to turn these off (the
// same switch as for replies to comments)
Rooftree::Server::MailMessage MentionMail(
    UserRow const& to,
    MessageRowData const& msg,
    std::string const& chatName) {
  BlogSettings const& settings = sWorld->GetSettings();
  std::string lang = to.Locale == "en" ? "en" : "zh";
  std::string site = sWorld->GetSiteName(lang);
  std::string link =
      Rooftree::StringFormat("{}/{}/chat?c={}", settings.PublicOrigin, lang, msg.ChannelId);
  std::string account = Rooftree::StringFormat("{}/{}/account", settings.PublicOrigin, lang);
  std::string quoted;
  for (std::string_view line : Rooftree::Tokenize(Excerpt(msg.Content, 300), '\n', true))
    quoted += "> " + std::string(line) + "\n";

  if (lang == "en") {
    return {
        .To = to.Email,
        .Subject = msg.SenderName + " mentioned you on " + site,
        .TextBody = Rooftree::StringFormat(
            "Hi {},\n\n{} mentioned you in {}:\n\n{}\nRead it and answer:\n{}\n\n--\n"
            "You get these as {} on {}. Turn them off in your account: {}\n",
            to.DisplayName,
            msg.SenderName,
            chatName,
            quoted,
            link,
            to.Username,
            site,
            account)};
  }
  return {
      .To = to.Email,
      .Subject = msg.SenderName + " 在" + site + "的聊天里提到了你",
      .TextBody = Rooftree::StringFormat(
          "{}，你好：\n\n{} 在「{}」里提到了你：\n\n{}\n去看看并回复：\n{}\n\n--\n"
          "你以 {} 的身份在{}收到这封邮件。不想再收到的话，在账号设置里关掉：{}\n",
          to.DisplayName,
          msg.SenderName,
          chatName,
          quoted,
          link,
          to.Username,
          site,
          account)};
}

// Worker: the chat's name as its readers see it, for mails: a direct chat's is the sender's
std::string WorkerChatName(std::string const& channelId, std::string const& senderName) {
  if (channelId == SquareChannel)
    return "小屋广场";
  if (Optional<uint32> groupId = GroupIdOf(channelId)) {
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_GROUP);
    stmt->setUInt32(0, *groupId);
    if (PreparedQueryResult row = BlogDatabase.Query(stmt))
      return row->Fetch()[0].GetString();
  }
  return senderName;
}

// A message to post: text, maybe a reply, maybe a file
struct NewMessage {
  std::string ChannelId;
  std::string Content;
  uint64 ReplyToId = 0;
  std::string FileName;  // empty: no file
  std::string FileMime;
  std::string FileData;
};

struct PostResult {
  std::string Error;
  Optional<MessageRowData> Message;
  std::vector<uint32> Readers;
  std::vector<UserRow> Mentioned;  // to mail when away
  std::string ChatName;
  uint64 TotalMessages = 0;
  uint32 TotalChannels = 0;
};

// Worker: the message stored (its file too), the sender's cursor past it, the oldest over the
// chat's limit gone
PostResult WorkerPostMessage(
    uint32 callerId,
    NewMessage const& in,
    std::vector<std::string> const& mentionNames,
    std::string const& directory) {
  ChannelAccess access = WorkerChannelAccess(callerId, in.ChannelId);
  if (!access.Error.empty())
    return {.Error = access.Error};

  // A reply quotes a message of the same chat, still there
  if (in.ReplyToId) {
    BlogDatabasePreparedStatement* stmt =
        BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGE_META);
    stmt->setUInt64(0, in.ReplyToId);
    PreparedQueryResult row = BlogDatabase.Query(stmt);
    if (!row || row->Fetch()[0].GetStringView() != in.ChannelId)
      return {.Error = "invalid_reply"};
  }

  std::string stored;
  if (!in.FileName.empty()) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    stored = Rooftree::Server::GenerateToken(16);
    std::ofstream out(directory + "/" + stored, std::ios::binary);
    out.write(in.FileData.data(), std::streamsize(in.FileData.size()));
    out.flush();
    if (!out) {
      RT_LOG_ERROR("blog.chat", "Could not write a chat file into {}", directory);
      RemoveChatFile(directory, stored);
      return {.Error = "internal_error"};
    }
  }

  BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_CHAT_MESSAGE);
  stmt->setString(0, in.ChannelId);
  stmt->setUInt32(1, callerId);
  stmt->setString(2, in.Content);
  if (in.ReplyToId) {
    stmt->setUInt64(3, in.ReplyToId);
  } else {
    stmt->setNull(3);
  }
  if (stored.empty()) {
    for (uint8 index = 4; index < 8; ++index)
      stmt->setNull(index);
  } else {
    stmt->setString(4, in.FileName);
    stmt->setString(5, in.FileMime);
    stmt->setUInt64(6, in.FileData.size());
    stmt->setString(7, stored);
  }
  uint64 messageId = BlogDatabase.DirectInsert(stmt);
  if (!messageId) {
    RemoveChatFile(directory, stored);
    return {.Error = "internal_error"};
  }

  std::vector<uint32> mentioned =
      WorkerResolveMentions(mentionNames, in.ChannelId, access.Readers, callerId);
  for (uint32 userId : mentioned) {
    stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_CHAT_MENTION);
    stmt->setUInt64(0, messageId);
    stmt->setUInt32(1, userId);
    stmt->setString(2, in.ChannelId);
    BlogDatabase.DirectExecute(stmt);
  }

  Optional<MessageRowData> msg = WorkerMessage(messageId);
  if (!msg)
    return {.Error = "internal_error"};

  // The sender has read what they sent
  stmt = BlogDatabase.GetPreparedStatement(BLOG_UPS_CHAT_READ);
  stmt->setUInt32(0, callerId);
  stmt->setString(1, in.ChannelId);
  stmt->setUInt64(2, msg->Id);
  stmt->setString(3, in.ChannelId);
  BlogDatabase.DirectExecute(stmt);

  // Only the newest so many are kept, their files with them
  stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_THRESHOLD_FIFO);
  stmt->setString(0, in.ChannelId);
  stmt->setUInt32(1, in.ChannelId.starts_with("dm:") ? DirectMessageLimit : SquareMessageLimit);
  if (PreparedQueryResult thresholdRow = BlogDatabase.Query(stmt)) {
    uint64 thresholdId = thresholdRow->Fetch()[0].GetUInt64();
    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_FILES_FIFO);
    stmt->setString(0, in.ChannelId);
    stmt->setUInt64(1, thresholdId);
    WorkerRemoveSelectedFiles(stmt, directory);
    stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_FIFO);
    stmt->setString(0, in.ChannelId);
    stmt->setUInt64(1, thresholdId);
    BlogDatabase.DirectExecute(stmt);
  }

  PostResult result = {.Message = std::move(msg), .Readers = std::move(access.Readers)};
  for (uint32 userId : mentioned) {
    if (Optional<UserRow> user = FindUserById(userId))
      result.Mentioned.push_back(std::move(*user));
  }
  if (!result.Mentioned.empty())
    result.ChatName = WorkerChatName(in.ChannelId, result.Message->SenderName);

  // For the status page
  if (PreparedQueryResult statsRow =
          BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_STATS))) {
    Field* f = statsRow->Fetch();
    result.TotalMessages = f[0].GetUInt64();
    result.TotalChannels = f[1].GetUInt32();
  }
  return result;
}

// Main thread: a message posted reaches its readers' pages (the sender's other pages too), the ones
// @mentioned and away get a mail
void AnnouncePosted(Session& session, PostResult const& res, uint32 callerId) {
  MessageRowData const& msg = *res.Message;
  sWorld->GetServerStatus().SetChatStats(res.TotalMessages, res.TotalChannels);

  SendToReaders(msg.ChannelId, res.Readers, "chat_message", MessageJson(msg));
  sWorld->GetPresence().Changed("chat:" + msg.ChannelId, "message");
  for (uint32 userId : res.Readers) {
    if (userId != callerId)
      sWorld->GetPresence().Changed("user:" + std::to_string(userId), "chat_unread");
  }
  for (UserRow const& user : res.Mentioned) {
    if (msg.ChannelId == SquareChannel)
      sWorld->GetPresence().Changed("user:" + std::to_string(user.Id), "chat_unread");
    bool away = !sWorld->GetPresence().FindOnlineUser(user.Id).has_value();
    if (away && !user.Deleted && user.EmailVerified && user.ReplyMails &&
        sWorld->GetReplyMailLimiter().Allow(user.Id))
      sWorld->SendMail(session, MentionMail(user, msg, res.ChatName));
  }
}

// Main thread: a message changed (edited, taken back) reaches its readers' pages
void AnnounceUpdated(MessageRowData const& msg, std::vector<uint32> const& readers) {
  SendToReaders(msg.ChannelId, readers, "chat_updated", MessageJson(msg));
  sWorld->GetPresence().Changed("chat:" + msg.ChannelId, "message");
  for (uint32 userId : readers)
    sWorld->GetPresence().Changed("user:" + std::to_string(userId), "chat_unread");
}

// ---- the caller's chats

// Every chat the caller is in: the square, its groups (name, members, owner), its friends' (friend)
struct ChannelData {
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
  uint32 MentionCount = 0;
  uint32 MemberCount = 0;
  uint32 OwnerId = 0;
  bool Pinned = false;
  bool Muted = false;
};

std::vector<ChannelData> WorkerCallerChannels(uint32 callerId) {
  std::vector<ChannelData> channels;
  channels.push_back({.Id = SquareChannel, .Type = "square", .Name = "小屋广场"});

  BlogDatabasePreparedStatement* stmt =
      BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_CHAT_GROUPS);
  stmt->setUInt32(0, callerId);
  if (PreparedQueryResult rows = BlogDatabase.Query(stmt)) {
    do {
      Field* fields = rows->Fetch();
      channels.push_back(
          {.Id = GroupChannel(fields[0].GetUInt32()),
           .Type = "group",
           .Name = fields[1].GetString(),
           .MemberCount = fields[3].GetUInt32(),
           .OwnerId = fields[2].GetUInt32()});
    } while (rows->NextRow());
  }

  stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FRIENDS);
  stmt->setUInt32(0, callerId);
  if (PreparedQueryResult rows = BlogDatabase.Query(stmt)) {
    do {
      Field* fields = rows->Fetch();
      uint32 friendId = fields[0].GetUInt32();
      channels.push_back(
          {.Id = MakeDmChannelId(callerId, friendId),
           .Type = "direct",
           .Name = fields[2].GetString(),
           .FriendId = friendId,
           .FriendUsername = fields[1].GetString(),
           .FriendDisplayName = fields[2].GetString(),
           .FriendAvatar = fields[3].GetStringOrNull().value_or(""),
           .FriendSince = fields[4].GetDate(),
           .MemberCount = 2});
    } while (rows->NextRow());
  }

  // The caller's settings of them
  stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_PREFS);
  stmt->setUInt32(0, callerId);
  if (PreparedQueryResult rows = BlogDatabase.Query(stmt)) {
    do {
      Field* fields = rows->Fetch();
      auto found = std::ranges::find(channels, fields[0].GetStringView(), &ChannelData::Id);
      if (found != channels.end()) {
        found->Pinned = fields[1].GetBool();
        found->Muted = fields[2].GetBool();
      }
    } while (rows->NextRow());
  }

  // How many the caller hasn't read, and how many of those mention it
  for (ChannelData& channel : channels) {
    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_UNREAD);
    stmt->setString(0, channel.Id);
    stmt->setUInt32(1, callerId);
    stmt->setString(2, channel.Id);
    if (PreparedQueryResult row = BlogDatabase.Query(stmt))
      channel.UnreadCount = row->Fetch()[0].GetUInt32();
    if (!channel.UnreadCount)
      continue;
    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MENTION_UNREAD);
    stmt->setUInt32(0, callerId);
    stmt->setString(1, channel.Id);
    stmt->setUInt32(2, callerId);
    stmt->setString(3, channel.Id);
    if (PreparedQueryResult row = BlogDatabase.Query(stmt))
      channel.MentionCount = row->Fetch()[0].GetUInt32();
  }
  return channels;
}
}  // namespace

std::string ChatFileDirectory(std::string const& uploadDirectory) {
  return uploadDirectory + "/chat";
}

void WorkerDeleteUserChatFiles(uint32 userId, std::string const& uploadDirectory) {
  BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_USER_FILES);
  stmt->setUInt32(0, userId);
  WorkerRemoveSelectedFiles(stmt, ChatFileDirectory(uploadDirectory));
}

// GET /api/chat/channels
Rooftree::Task<void> ListChatChannels(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;

  std::vector<ChannelData> channels = co_await session.OnWorker([callerId]() {
    std::vector<ChannelData> channels = WorkerCallerChannels(callerId);
    for (ChannelData& channel : channels) {
      BlogDatabasePreparedStatement* stmt =
          BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_LAST_MESSAGE);
      stmt->setString(0, channel.Id);
      if (PreparedQueryResult row = BlogDatabase.Query(stmt))
        channel.LastMessage = FetchMessageRow(row->Fetch());
    }
    return channels;
  });

  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<Reader::ChatChannelItem>> items;
  items.reserve(channels.size());
  for (ChannelData const& ch : channels) {
    flatbuffers::Offset<Reader::UserFriendItem> friendItem = 0;
    if (ch.Type == "direct") {
      bool online = sWorld->GetPresence().FindOnlineUser(ch.FriendId).has_value();
      auto friendAvatar =
          ch.FriendAvatar.empty() ? 0 : builder.CreateString(AvatarUrl(ch.FriendAvatar));
      friendItem = Reader::CreateUserFriendItem(
          builder, ch.FriendId, builder.CreateString(ch.FriendUsername),
          builder.CreateString(ch.FriendDisplayName), friendAvatar, online,
          TimeString(builder, ch.FriendSince));
    }

    flatbuffers::Offset<Reader::ChatMessageItem> lastMsg = 0;
    if (ch.LastMessage)
      lastMsg = BuildMessageItem(builder, *ch.LastMessage);

    items.push_back(
        Reader::CreateChatChannelItem(
            builder,
            builder.CreateString(ch.Id),
            builder.CreateString(ch.Type),
            builder.CreateString(ch.Name),
            0,
            friendItem,
            lastMsg,
            ch.UnreadCount,
            ch.MemberCount,
            ch.MentionCount,
            ch.Pinned,
            ch.Muted,
            ch.OwnerId));
  }

  builder.Finish(Reader::CreateChatChannelsList(builder, builder.CreateVector(items)));
  request->Respond(builder);
}

// GET /api/chat/channels/{channelId}/messages?before=&limit=
Rooftree::Task<void> ListChatMessages(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  Reader::ChatMessagesListRequest const& in = request->Get<Reader::ChatMessagesListRequest>();

  std::string channelId = NormalizeChannel(in.channelId() ? in.channelId()->str() : "");
  if (channelId.empty())
    co_return request->RespondError(status::bad_request, "channel_required");

  uint64 before = in.before().value_or(0);
  // Pages of 50, or of up to 200 when going back far (to a message found by a search)
  uint32 limit = in.limit().value_or(50);
  if (limit == 0 || limit > 200)
    limit = 50;

  Optional<std::pair<std::vector<MessageRowData>, bool>> res = co_await session.OnWorker(
      [callerId, channelId = std::move(channelId), before,
       limit]() -> Optional<std::pair<std::vector<MessageRowData>, bool>> {
        if (!WorkerChannelAccess(callerId, channelId).Error.empty())
          return {};

        BlogDatabasePreparedStatement* stmt;
        if (before > 0) {
          stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGES_BEFORE);
          stmt->setString(0, channelId);
          stmt->setUInt64(1, before);
          stmt->setUInt32(2, limit);
        } else {
          stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGES_INITIAL);
          stmt->setString(0, channelId);
          stmt->setUInt32(1, limit);
        }
        std::vector<MessageRowData> messages = WorkerMessages(stmt);
        bool hasMore = messages.size() == limit;
        return std::make_pair(std::move(messages), hasMore);
      });
  if (!res)
    co_return request->RespondError(status::forbidden, "not_allowed");

  auto& [messages, hasMore] = *res;
  flatbuffers::FlatBufferBuilder builder;

  // Oldest first
  std::vector<flatbuffers::Offset<Reader::ChatMessageItem>> items;
  items.reserve(messages.size());
  for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    items.push_back(BuildMessageItem(builder, *it));

  builder.Finish(Reader::CreateChatMessagesList(builder, builder.CreateVector(items), hasMore));
  request->Respond(builder);
}

// POST /api/chat/channels/{channelId}/messages
Rooftree::Task<void> SendChatMessage(Session& session, ApiRequest request) {
  AuthInfo const& auth = request->GetAuth();
  uint32 callerId = auth.User.Id;

  if (auth.User.IsDenied(DenyChat))
    co_return request->RespondError(status::forbidden, "not_allowed");

  Reader::ChatMessageSendRequest const& in = request->Get<Reader::ChatMessageSendRequest>();
  TableFields fields(request->GetErrors());
  NewMessage message;
  message.ChannelId =
      NormalizeChannel(fields.Text("channelId", in.channelId(), 1, MaxChannelIdLength));
  message.Content = fields.Text("content", in.content(), 1, MaxMessageLength, true, true);
  message.ReplyToId = in.replyToId().value_or(0);
  if (!fields.IsValid())
    co_return RespondValidation(request, fields.TakeErrors());

  std::vector<std::string> mentionNames = MentionedNames(message.Content);
  PostResult res = co_await session.OnWorker(
      [callerId, message = std::move(message), mentionNames = std::move(mentionNames),
       directory = ChatFileDirectory(sWorld->GetSettings().UploadDirectory)] {
        return WorkerPostMessage(callerId, message, mentionNames, directory);
      });
  if (res.Error == "internal_error")
    co_return RespondInternalError(request);
  if (res.Error == "invalid_reply")
    co_return request->RespondError(status::bad_request, res.Error);
  if (!res.Error.empty())
    co_return RespondAccessError(request, res.Error);

  AnnouncePosted(session, res, callerId);
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(BuildMessageItem(builder, *res.Message));
  request->Respond(builder);
}

// POST /api/chat/channels/{channelId}/files: multipart, a file and a caption
Rooftree::Task<void> SendChatFile(Session& session, ApiRequest request) {
  AuthInfo const& auth = request->GetAuth();
  uint32 callerId = auth.User.Id;

  if (auth.User.IsDenied(DenyChat))
    co_return request->RespondError(status::forbidden, "not_allowed");

  Reader::ChatFileSendRequest const& in = request->Get<Reader::ChatFileSendRequest>();
  TableFields fields(request->GetErrors());
  NewMessage message;
  message.ChannelId =
      NormalizeChannel(fields.Text("channelId", in.channelId(), 1, MaxChannelIdLength));
  message.Content = fields.Text("content", in.content(), 0, MaxMessageLength, false, true);
  message.ReplyToId = in.replyToId().value_or(0);

  // Its name as sent, without a path; "file" when there's none left
  std::string name = in.name() ? in.name()->str() : std::string();
  if (std::size_t slash = name.find_last_of("/\\"); slash != std::string::npos)
    name.erase(0, slash + 1);
  std::erase_if(name, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
  if (name.empty() || name == "." || name == "..")
    name = "file";
  if (CountCharacters(name) > MaxFileNameLength)
    fields.AddError("name", "too_long");

  if (!in.file() || !in.file()->size()) {
    fields.AddError("file", "required");
  } else if (in.file()->size() > sWorld->GetSettings().MaxUploadSize) {
    fields.AddError("file", "too_long");
  }
  if (!fields.IsValid())
    co_return RespondValidation(request, fields.TakeErrors());

  message.FileData.assign(reinterpret_cast<char const*>(in.file()->data()), in.file()->size());
  // A picture by its bytes, whatever it is called;
  // anything else by its name, never shown by the browser
  Optional<ImageType> image = DetectImage(message.FileData);
  message.FileMime = image ? std::string(image->Mime) : Drive::MimeOf(name);
  if (!image && IsChatImage(message.FileMime))
    message.FileMime = "application/octet-stream";
  message.FileName = std::move(name);

  std::vector<std::string> mentionNames = MentionedNames(message.Content);
  PostResult res = co_await session.OnWorker(
      [callerId, message = std::move(message), mentionNames = std::move(mentionNames),
       directory = ChatFileDirectory(sWorld->GetSettings().UploadDirectory)] {
        return WorkerPostMessage(callerId, message, mentionNames, directory);
      });
  if (res.Error == "internal_error")
    co_return RespondInternalError(request);
  if (res.Error == "invalid_reply")
    co_return request->RespondError(status::bad_request, res.Error);
  if (!res.Error.empty())
    co_return RespondAccessError(request, res.Error);

  AnnouncePosted(session, res, callerId);
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(BuildMessageItem(builder, *res.Message));
  request->Respond(builder);
}

namespace {
// What the sender may still do to its own message: edit or take it back, within a window
struct OwnMessageChange {
  std::string
      Error;  // not_found, not_allowed, recalled, too_late, internal_error, or an access error
  Optional<MessageRowData> Message;
  std::vector<uint32> Readers;
};

// Worker: the caller's own message, still in a chat it may read, sent less than window ago
OwnMessageChange WorkerOwnMessage(
    uint32 callerId,
    uint64 messageId,
    Seconds window,
    std::string& stored) {
  BlogDatabasePreparedStatement* stmt =
      BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGE_META);
  stmt->setUInt64(0, messageId);
  PreparedQueryResult row = BlogDatabase.Query(stmt);
  if (!row)
    return {.Error = "not_found"};
  Field* fields = row->Fetch();
  std::string channelId = fields[0].GetString();
  ChannelAccess access = WorkerChannelAccess(callerId, channelId);
  if (!access.Error.empty())
    return {.Error = "not_found"};
  if (fields[1].GetUInt32() != callerId)
    return {.Error = "not_allowed"};
  if (fields[3].GetBool())
    return {.Error = "recalled"};
  if (SystemTimePoint::clock::now() - fields[2].GetDate() > window)
    return {.Error = "too_late"};
  stored = fields[4].GetStringOrNull().value_or("");
  return {.Readers = std::move(access.Readers)};
}

void RespondOwnMessageError(ApiRequest const& request, std::string const& error) {
  if (error == "internal_error")
    return RespondInternalError(request);
  if (error == "not_found")
    return RespondNotFound(request);
  request->RespondError(error == "not_allowed" ? status::forbidden : status::conflict, error);
}
}  // namespace

// PATCH /api/chat/messages/{id}: its text, by its sender, within Blog.Chat.EditWindow
Rooftree::Task<void> EditChatMessage(Session& session, ApiRequest request) {
  AuthInfo const& auth = request->GetAuth();
  uint32 callerId = auth.User.Id;
  if (auth.User.IsDenied(DenyChat))
    co_return request->RespondError(status::forbidden, "not_allowed");

  Reader::ChatMessageEditRequest const& in = request->Get<Reader::ChatMessageEditRequest>();
  TableFields fields(request->GetErrors());
  // A file's caption may be emptied, a text not: checked against the message below
  std::string content = fields.Text("content", in.content(), 0, MaxMessageLength, false, true);
  if (!fields.IsValid())
    co_return RespondValidation(request, fields.TakeErrors());

  OwnMessageChange res = co_await session.OnWorker([callerId, id = in.id(), content,
                                                    window = sWorld->GetSettings().ChatEditWindow] {
    std::string stored;
    OwnMessageChange change = WorkerOwnMessage(callerId, id, window, stored);
    if (!change.Error.empty())
      return change;
    if (content.empty() && stored.empty())
      return OwnMessageChange{.Error = "empty"};

    BlogDatabasePreparedStatement* stmt =
        BlogDatabase.GetPreparedStatement(BLOG_UPD_CHAT_MESSAGE_EDIT);
    stmt->setString(0, content);
    stmt->setUInt64(1, id);
    BlogDatabase.DirectExecute(stmt);
    change.Message = WorkerMessage(id);
    if (!change.Message)
      change.Error = "internal_error";
    return change;
  });
  if (res.Error == "empty")
    co_return RespondValidation(request, {{"content", "required"}});
  if (!res.Error.empty())
    co_return RespondOwnMessageError(request, res.Error);

  AnnounceUpdated(*res.Message, res.Readers);
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(BuildMessageItem(builder, *res.Message));
  request->Respond(builder);
}

// POST /api/chat/messages/{id}/recall: taken back by its sender within Blog.Chat.RecallWindow. It
// stays as "taken back", without its text, file or mentions
Rooftree::Task<void> RecallChatMessage(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  uint64 id = request->Get<Reader::ChatMessageRequest>().id();

  OwnMessageChange res = co_await session.OnWorker(
      [callerId, id, window = sWorld->GetSettings().ChatRecallWindow,
       directory = ChatFileDirectory(sWorld->GetSettings().UploadDirectory)] {
        std::string stored;
        OwnMessageChange change = WorkerOwnMessage(callerId, id, window, stored);
        if (!change.Error.empty())
          return change;

        BlogDatabasePreparedStatement* stmt =
            BlogDatabase.GetPreparedStatement(BLOG_UPD_CHAT_MESSAGE_RECALL);
        stmt->setUInt64(0, id);
        BlogDatabase.DirectExecute(stmt);
        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_MESSAGE_MENTIONS);
        stmt->setUInt64(0, id);
        BlogDatabase.DirectExecute(stmt);
        RemoveChatFile(directory, stored);
        change.Message = WorkerMessage(id);
        if (!change.Message)
          change.Error = "internal_error";
        return change;
      });
  if (!res.Error.empty())
    co_return RespondOwnMessageError(request, res.Error);

  AnnounceUpdated(*res.Message, res.Readers);
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(BuildMessageItem(builder, *res.Message));
  request->Respond(builder);
}

// GET /api/chat/search?q=&channelId=&before=: newest first, SearchPageSize a page
Rooftree::Task<void> SearchChat(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  Reader::ChatSearchRequest const& in = request->Get<Reader::ChatSearchRequest>();
  TableFields fields(request->GetErrors());
  std::string q = fields.Text("q", in.q(), 1, MaxSearchLength);
  std::string channelId =
      NormalizeChannel(fields.Text("channelId", in.channelId(), 0, MaxChannelIdLength, false));
  uint64 before = in.before().value_or(0);
  if (!fields.IsValid())
    co_return RespondValidation(request, fields.TakeErrors());

  // %, _ and \ are themselves
  std::string pattern = "%";
  for (char c : q) {
    if (c == '%' || c == '_' || c == '\\')
      pattern += '\\';
    pattern += c;
  }
  pattern += '%';

  struct SearchResult {
    std::string Error;
    std::vector<MessageRowData> Messages;
  };
  SearchResult res = co_await session.OnWorker([callerId, pattern, channelId, before] {
    if (!channelId.empty()) {
      if (ChannelAccess access = WorkerChannelAccess(callerId, channelId); !access.Error.empty())
        return SearchResult{.Error = access.Error};
    }

    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_SEARCH);
    stmt->setString(0, pattern);
    stmt->setString(1, pattern);
    stmt->setUInt64(2, before ? before : std::numeric_limits<int64>::max());
    stmt->setString(3, channelId);
    stmt->setString(4, channelId);
    stmt->setUInt32(5, callerId);
    stmt->setUInt32(6, callerId);
    return SearchResult{.Messages = WorkerMessages(stmt)};
  });
  if (!res.Error.empty())
    co_return RespondAccessError(request, res.Error);

  // The 51st only says there is more
  bool more = res.Messages.size() > SearchPageSize;
  if (more)
    res.Messages.resize(SearchPageSize);
  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<Reader::ChatMessageItem>> items;
  for (MessageRowData const& msg : res.Messages)
    items.push_back(BuildMessageItem(builder, msg));
  builder.Finish(Reader::CreateChatSearchResult(builder, builder.CreateVector(items), more));
  request->Respond(builder);
}

// PUT /api/chat/channels/{channelId}/prefs: pinned, muted; what isn't sent stays
Rooftree::Task<void> SetChatChannelPref(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  Reader::ChatChannelPrefRequest const& in = request->Get<Reader::ChatChannelPrefRequest>();
  std::string channelId = NormalizeChannel(in.channelId() ? in.channelId()->str() : "");
  Optional<bool> pinned = in.pinned() ? Optional<bool>(*in.pinned()) : Optional<bool>();
  Optional<bool> muted = in.muted() ? Optional<bool>(*in.muted()) : Optional<bool>();
  if (channelId.empty() || channelId.size() > MaxChannelIdLength)
    co_return request->RespondError(status::bad_request, "invalid_channel");

  std::string error = co_await session.OnWorker([callerId, channelId, pinned, muted] {
    ChannelAccess access = WorkerChannelAccess(callerId, channelId);
    if (!access.Error.empty())
      return access.Error;

    bool wasPinned = false, wasMuted = false;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_PREF);
    stmt->setUInt32(0, callerId);
    stmt->setString(1, channelId);
    if (PreparedQueryResult row = BlogDatabase.Query(stmt)) {
      wasPinned = row->Fetch()[0].GetBool();
      wasMuted = row->Fetch()[1].GetBool();
    }

    stmt = BlogDatabase.GetPreparedStatement(BLOG_UPS_CHAT_PREF);
    stmt->setUInt32(0, callerId);
    stmt->setString(1, channelId);
    stmt->setBool(2, pinned.value_or(wasPinned));
    stmt->setBool(3, muted.value_or(wasMuted));
    BlogDatabase.DirectExecute(stmt);
    return std::string();
  });
  if (!error.empty())
    co_return RespondAccessError(request, error);

  // The caller's other pages
  TellChannelsChanged({callerId});
  sWorld->GetPresence().Changed("user:" + std::to_string(callerId), "chat_unread");
  RespondOk(request);
}

// GET /api/chat/unread: the unread of the chats that
// aren't muted, and the mentions of those that are
Rooftree::Task<void> GetChatUnreadCount(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;

  uint32 count = co_await session.OnWorker([callerId]() {
    uint32 total = 0;
    for (ChannelData const& channel : WorkerCallerChannels(callerId))
      total += channel.Muted ? channel.MentionCount : channel.UnreadCount;
    return total;
  });
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(Reader::CreateChatUnreadCount(builder, count));
  request->Respond(builder);
}

// POST /api/chat/channels/{channelId}/read
Rooftree::Task<void> MarkChatRead(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  Reader::ChatMarkReadRequest const& in = request->Get<Reader::ChatMarkReadRequest>();

  std::string channelId = NormalizeChannel(in.channelId() ? in.channelId()->str() : "");
  uint64 lastReadId = in.lastReadId();

  if (channelId.empty() || channelId.size() > MaxChannelIdLength || lastReadId == 0)
    co_return RespondOk(request);

  // Only a channel the caller may read: no read cursors for others' chats. The cursor never passes
  // the channel's newest message, so a bogus id can't hide the messages still to come
  bool marked =
      co_await session.OnWorker([callerId, channelId = std::move(channelId), lastReadId]() {
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

// GET /api/chat/files/{id}: a message's file, to those who may read its chat. A picture is shown,
// the rest downloaded; nothing of it runs as our page
Rooftree::Task<void> BlogSession::HandleChatFile(Request request) {
  Optional<AuthInfo> login = co_await RequireUserAsync(request);
  if (!login)
    co_return;
  uint32 callerId = login->User.Id;

  Optional<uint64> id = Rooftree::StringTo<uint64>(request->GetPathParameter("id"));
  if (!id || !*id)
    co_return RespondNotFound(request);

  struct ChatFile {
    std::string Name;
    std::string Mime;
    uint64 Size = 0;
    std::shared_ptr<std::ifstream> Stream;
  };
  Optional<ChatFile> file = co_await OnWorker(
      [callerId, id = *id, directory = ChatFileDirectory(sWorld->GetSettings().UploadDirectory)]()
          -> Optional<ChatFile> {
        BlogDatabasePreparedStatement* stmt =
            BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGE_META);
        stmt->setUInt64(0, id);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row || row->Fetch()[4].IsNull())
          return {};
        Field* fields = row->Fetch();
        if (!WorkerChannelAccess(callerId, fields[0].GetString()).Error.empty())
          return {};

        std::string stored = fields[4].GetString();
        ChatFile file{fields[5].GetString(), fields[6].GetString(), fields[7].GetUInt64()};
        if (stored.find('/') == std::string::npos) {
          file.Stream = std::make_shared<std::ifstream>(directory + "/" + stored, std::ios::binary);
          if (!*file.Stream)
            file.Stream.reset();
        }
        return file;
      });
  if (!file)
    co_return RespondNotFound(request);
  if (!file->Stream) {
    RT_LOG_ERROR("blog.chat", "The file of chat message {} is missing", *id);
    co_return RespondInternalError(request);
  }

  bool shown = IsChatImage(file->Mime) &&
               request->GetQueryParameter("download").value_or("") != "1";
  http::response<http::string_body>& response = request->GetResponse();
  response.set(http::field::cache_control, "private, max-age=86400");
  response.set("X-Content-Type-Options", "nosniff");
  response.set(http::field::content_type, shown ? file->Mime : "application/octet-stream");
  response.set(http::field::content_disposition, Drive::ContentDisposition(!shown, file->Name));
  response.set(http::field::content_security_policy, "sandbox; frame-ancestors 'self'");
  request->RespondStream(status::ok, file->Stream, file->Size);
}

// POST /api/chat/groups
Rooftree::Task<void> CreateChatGroup(Session& session, ApiRequest request) {
  AuthInfo const& auth = request->GetAuth();
  uint32 callerId = auth.User.Id;

  if (auth.User.IsDenied(DenyChat))
    co_return request->RespondError(status::forbidden, "not_allowed");

  Reader::ChatGroupCreateRequest const& in = request->Get<Reader::ChatGroupCreateRequest>();
  TableFields fields(request->GetErrors());
  std::string name = fields.Text("name", in.name(), 1, MaxGroupNameLength);

  std::vector<uint32> memberIds;
  if (in.memberIds()) {
    for (uint32 id : *in.memberIds()) {
      if (id && id != callerId && std::ranges::find(memberIds, id) == memberIds.end())
        memberIds.push_back(id);
    }
  }
  if (memberIds.size() + 1 > MaxGroupMembers)
    fields.AddError("memberIds", "too_long");
  if (!fields.IsValid())
    co_return RespondValidation(request, fields.TakeErrors());

  struct GroupResult {
    std::string Error;
    uint32 GroupId = 0;
    std::vector<uint32> Members;
  };

  GroupResult res = co_await session.OnWorker(
      [callerId, name, memberIds = std::move(memberIds)]() -> GroupResult {
        // Only friends can be put in
        for (uint32 memberId : memberIds) {
          if (!WorkerAreFriends(callerId, memberId))
            return {.Error = "not_friends"};
        }

        BlogDatabasePreparedStatement* stmt =
            BlogDatabase.GetPreparedStatement(BLOG_INS_CHAT_GROUP);
        stmt->setString(0, name);
        stmt->setUInt32(1, callerId);
        uint32 groupId = uint32(BlogDatabase.DirectInsert(stmt));
        if (!groupId)
          return {.Error = "internal_error"};

        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
        std::vector<uint32> members = memberIds;
        members.push_back(callerId);
        for (uint32 memberId : members) {
          stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_CHAT_GROUP_MEMBER);
          stmt->setUInt32(0, groupId);
          stmt->setUInt32(1, memberId);
          stmt->setString(2, std::string(memberId == callerId ? "owner" : "member"));
          trans->Append(stmt);
        }
        BlogDatabase.DirectCommitTransaction(trans);
        return {.GroupId = groupId, .Members = std::move(members)};
      });
  if (res.Error == "internal_error")
    co_return RespondInternalError(request);
  if (!res.Error.empty())
    co_return request->RespondError(status::forbidden, res.Error);

  TellChannelsChanged(res.Members);

  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(
      Reader::CreateChatChannelItem(
          builder,
          builder.CreateString(GroupChannel(res.GroupId)),
          builder.CreateString("group"),
          builder.CreateString(name),
          0,
          0,
          0,
          0,
          uint32(res.Members.size()),
          0,
          false,
          false,
          callerId));
  request->Respond(builder);
}

// GET /api/chat/groups/{id}/members
Rooftree::Task<void> ListChatGroupMembers(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  uint32 groupId = request->Get<Reader::ChatGroupActionRequest>().id();

  struct MemberData {
    uint32 Id = 0;
    std::string Username;
    std::string DisplayName;
    std::string Avatar;
    std::string Role;
  };

  Optional<std::vector<MemberData>> members =
      co_await session.OnWorker([callerId, groupId]() -> Optional<std::vector<MemberData>> {
        if (!WorkerIsGroupMember(groupId, callerId))
          return {};

        std::vector<MemberData> members;
        BlogDatabasePreparedStatement* stmt =
            BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_GROUP_MEMBERS);
        stmt->setUInt32(0, groupId);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt)) {
          do {
            Field* f = rows->Fetch();
            members.push_back(
                {f[0].GetUInt32(), f[1].GetString(), f[2].GetString(),
                 f[3].GetStringOrNull().value_or(""), f[4].GetString()});
          } while (rows->NextRow());
        }
        return members;
      });
  if (!members)
    co_return request->RespondError(status::forbidden, "not_allowed");

  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<Reader::ChatGroupMemberItem>> items;
  items.reserve(members->size());
  for (MemberData const& m : *members) {
    bool online = sWorld->GetPresence().FindOnlineUser(m.Id).has_value();
    auto avatar = m.Avatar.empty() ? 0 : builder.CreateString(AvatarUrl(m.Avatar));
    items.push_back(
        Reader::CreateChatGroupMemberItem(
            builder, m.Id, builder.CreateString(m.Username), builder.CreateString(m.DisplayName),
            avatar, builder.CreateString(m.Role), online));
  }

  builder.Finish(Reader::CreateChatGroupMembersList(builder, builder.CreateVector(items)));
  request->Respond(builder);
}

// POST /api/chat/groups/{id}/leave
// A member leaves; the owner leaving dissolves the group, its messages with it
Rooftree::Task<void> LeaveChatGroup(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  uint32 groupId = request->Get<Reader::ChatGroupActionRequest>().id();

  struct LeaveResult {
    bool Found = false;
    bool Dissolved = false;
    std::vector<uint32> Members;  // before leaving: the caller among them
  };

  LeaveResult res = co_await session.OnWorker(
      [callerId, groupId,
       directory = ChatFileDirectory(sWorld->GetSettings().UploadDirectory)]() -> LeaveResult {
        Optional<std::string> role = WorkerGroupRole(groupId, callerId);
        if (!role)
          return {};

        LeaveResult result = {
            .Found = true, .Dissolved = *role == "owner", .Members = WorkerGroupMembers(groupId)};
        if (result.Dissolved) {
          WorkerDeleteGroup(groupId, directory);
        } else {
          BlogDatabasePreparedStatement* stmt =
              BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_GROUP_MEMBER);
          stmt->setUInt32(0, groupId);
          stmt->setUInt32(1, callerId);
          BlogDatabase.DirectExecute(stmt);
          stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_USER_PREF);
          stmt->setUInt32(0, callerId);
          stmt->setString(1, GroupChannel(groupId));
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

namespace {
// A group change only its owner may make: who is in it before (and after) the change
struct OwnerChange {
  std::string Error;            // group_not_found, not_owner, or the change's own
  std::vector<uint32> Members;  // before
  std::vector<uint32> Told;     // who is told: the members before and after
};

// Worker: change run if the caller owns the group, its error kept
OwnerChange WorkerAsOwner(
    uint32 callerId,
    uint32 groupId,
    std::function<std::string(std::vector<uint32>&)> const& change) {
  Optional<std::string> role = WorkerGroupRole(groupId, callerId);
  if (!role)
    return {.Error = "group_not_found"};
  if (*role != "owner")
    return {.Error = "not_owner"};
  OwnerChange result = {.Members = WorkerGroupMembers(groupId)};
  result.Told = result.Members;
  result.Error = change(result.Told);
  return result;
}

void RespondOwnerChangeError(ApiRequest const& request, std::string const& error) {
  if (error == "internal_error")
    return RespondInternalError(request);
  if (error == "group_not_found" || error == "not_member")
    return request->RespondError(status::not_found, error);
  if (error == "not_owner" || error == "not_friends")
    return request->RespondError(status::forbidden, error);
  request->RespondError(status::conflict, error);
}
}  // namespace

// PATCH /api/chat/groups/{id}: renamed by its owner
Rooftree::Task<void> RenameChatGroup(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  Reader::ChatGroupRenameRequest const& in = request->Get<Reader::ChatGroupRenameRequest>();
  TableFields fields(request->GetErrors());
  std::string name = fields.Text("name", in.name(), 1, MaxGroupNameLength);
  if (!fields.IsValid())
    co_return RespondValidation(request, fields.TakeErrors());

  OwnerChange res = co_await session.OnWorker([callerId, groupId = in.id(), name] {
    return WorkerAsOwner(callerId, groupId, [&](std::vector<uint32>&) {
      BlogDatabasePreparedStatement* stmt =
          BlogDatabase.GetPreparedStatement(BLOG_UPD_CHAT_GROUP_NAME);
      stmt->setString(0, name);
      stmt->setUInt32(1, groupId);
      BlogDatabase.DirectExecute(stmt);
      return std::string();
    });
  });
  if (!res.Error.empty())
    co_return RespondOwnerChangeError(request, res.Error);
  TellChannelsChanged(res.Told);
  RespondOk(request);
}

// POST /api/chat/groups/{id}/members: the owner's friends put in
Rooftree::Task<void> AddChatGroupMembers(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  Reader::ChatGroupMembersRequest const& in = request->Get<Reader::ChatGroupMembersRequest>();
  std::vector<uint32> memberIds;
  if (in.memberIds()) {
    for (uint32 id : *in.memberIds()) {
      if (id && id != callerId && std::ranges::find(memberIds, id) == memberIds.end())
        memberIds.push_back(id);
    }
  }
  if (memberIds.empty())
    co_return RespondValidation(request, {{"memberIds", "required"}});

  OwnerChange res =
      co_await session.OnWorker([callerId, groupId = in.id(), memberIds = std::move(memberIds)] {
        return WorkerAsOwner(callerId, groupId, [&](std::vector<uint32>& told) {
          std::vector<uint32> added;
          for (uint32 memberId : memberIds) {
            if (std::ranges::find(told, memberId) != told.end())
              continue;
            if (!WorkerAreFriends(callerId, memberId))
              return std::string("not_friends");
            added.push_back(memberId);
          }
          if (told.size() + added.size() > MaxGroupMembers)
            return std::string("group_full");

          BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
          for (uint32 memberId : added) {
            BlogDatabasePreparedStatement* stmt =
                BlogDatabase.GetPreparedStatement(BLOG_INS_CHAT_GROUP_MEMBER);
            stmt->setUInt32(0, groupId);
            stmt->setUInt32(1, memberId);
            stmt->setString(2, std::string("member"));
            trans->Append(stmt);
          }
          BlogDatabase.DirectCommitTransaction(trans);
          told.insert(told.end(), added.begin(), added.end());
          return std::string();
        });
      });
  if (!res.Error.empty())
    co_return RespondOwnerChangeError(request, res.Error);
  TellChannelsChanged(res.Told);
  RespondOk(request);
}

// DELETE /api/chat/groups/{id}/members/{userId}: a member put out by the owner
Rooftree::Task<void> RemoveChatGroupMember(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  Reader::ChatGroupMemberRequest const& in = request->Get<Reader::ChatGroupMemberRequest>();

  OwnerChange res = co_await session.OnWorker([callerId, groupId = in.id(), userId = in.userId()] {
    return WorkerAsOwner(callerId, groupId, [&](std::vector<uint32>&) {
      if (userId == callerId)
        return std::string("owner_cannot_leave");
      if (!WorkerIsGroupMember(groupId, userId))
        return std::string("not_member");
      BlogDatabasePreparedStatement* stmt =
          BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_GROUP_MEMBER);
      stmt->setUInt32(0, groupId);
      stmt->setUInt32(1, userId);
      BlogDatabase.DirectExecute(stmt);
      stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_USER_PREF);
      stmt->setUInt32(0, userId);
      stmt->setString(1, GroupChannel(groupId));
      BlogDatabase.DirectExecute(stmt);
      return std::string();
    });
  });
  if (!res.Error.empty())
    co_return RespondOwnerChangeError(request, res.Error);
  TellChannelsChanged(res.Told);
  RespondOk(request);
}

// POST /api/chat/groups/{id}/owner: the group handed to another member, the owner stays a member
Rooftree::Task<void> TransferChatGroup(Session& session, ApiRequest request) {
  uint32 callerId = request->GetAuth().User.Id;
  Reader::ChatGroupMemberRequest const& in = request->Get<Reader::ChatGroupMemberRequest>();

  OwnerChange res = co_await session.OnWorker([callerId, groupId = in.id(), userId = in.userId()] {
    return WorkerAsOwner(callerId, groupId, [&](std::vector<uint32>&) {
      if (userId == callerId)
        return std::string("already_owner");
      if (!WorkerIsGroupMember(groupId, userId))
        return std::string("not_member");
      BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
      BlogDatabasePreparedStatement* stmt =
          BlogDatabase.GetPreparedStatement(BLOG_UPD_CHAT_GROUP_OWNER);
      stmt->setUInt32(0, userId);
      stmt->setUInt32(1, groupId);
      trans->Append(stmt);
      stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_CHAT_GROUP_MEMBER_ROLE);
      stmt->setString(0, std::string("owner"));
      stmt->setUInt32(1, groupId);
      stmt->setUInt32(2, userId);
      trans->Append(stmt);
      stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_CHAT_GROUP_MEMBER_ROLE);
      stmt->setString(0, std::string("member"));
      stmt->setUInt32(1, groupId);
      stmt->setUInt32(2, callerId);
      trans->Append(stmt);
      BlogDatabase.DirectCommitTransaction(trans);
      return std::string();
    });
  });
  if (!res.Error.empty())
    co_return RespondOwnerChangeError(request, res.Error);
  TellChannelsChanged(res.Told);
  RespondOk(request);
}

// ---- moderation (Admin.fbs): the square and the groups, never the direct chats

namespace {
// A channel a moderator may look into: the square or a group's ("group:<id>")
bool IsModeratedChannel(std::string_view channelId) {
  return channelId == SquareChannel || GroupIdOf(channelId).has_value();
}

// Who reads it, for telling them what went: the square's are all logged in pages (SendToReaders)
std::vector<uint32> WorkerReadersOf(std::string const& channelId) {
  Optional<uint32> groupId = GroupIdOf(channelId);
  return groupId ? WorkerGroupMembers(*groupId) : std::vector<uint32>();
}

void RespondRemoved(ApiRequest const& request, uint32 removed) {
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(Admin::CreateChatRemoved(builder, removed));
  request->Respond(builder);
}
}  // namespace

// GET /api/admin/chat/groups
Rooftree::Task<void> AdminListChatGroups(Session& session, ApiRequest request) {
  struct GroupRow {
    uint32 Id = 0;
    std::string Name;
    uint32 OwnerId = 0;
    std::string OwnerUsername;
    uint32 Members = 0;
    uint32 Messages = 0;
    SystemTimePoint CreatedAt;
  };

  std::vector<GroupRow> groups = co_await session.OnWorker([] {
    std::vector<GroupRow> groups;
    if (PreparedQueryResult rows =
            BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_CHAT_GROUPS))) {
      do {
        Field* f = rows->Fetch();
        groups.push_back(
            {f[0].GetUInt32(), f[1].GetString(), f[2].GetUInt32(), f[3].GetString(),
             uint32(f[4].GetUInt64()), uint32(f[5].GetUInt64()), f[6].GetDate()});
      } while (rows->NextRow());
    }
    return groups;
  });
  flatbuffers::FlatBufferBuilder builder;
  std::vector<flatbuffers::Offset<Admin::ChatGroup>> items;
  for (GroupRow const& g : groups) {
    items.push_back(
        Admin::CreateChatGroup(
            builder, g.Id, builder.CreateString(g.Name), g.OwnerId,
            builder.CreateString(g.OwnerUsername), g.Members, g.Messages,
            TimeString(builder, g.CreatedAt)));
  }
  builder.Finish(Admin::CreateChatGroupsList(builder, builder.CreateVector(items)));
  request->Respond(builder);
}

// GET /api/admin/chat/messages?channel=&cursor=, newest first, 50 a page
Rooftree::Task<void> AdminListChatMessages(Session& session, ApiRequest request) {
  Admin::ChatMessagesListRequest const& in = request->Get<Admin::ChatMessagesListRequest>();
  TableFields fields(request->GetErrors());
  std::string channelId = fields.Text("channel", in.channel(), 1, MaxChannelIdLength);
  if (!channelId.empty() && !IsModeratedChannel(channelId))
    fields.AddError("channel", "invalid");
  uint64 before =
      (in.cursor() ? Rooftree::StringTo<uint64>(in.cursor()->string_view()) : Optional<uint64>())
          .value_or(std::numeric_limits<uint64>::max());
  if (!fields.IsValid())
    co_return RespondValidation(request, fields.TakeErrors());

  struct Row {
    uint64 Id = 0;
    std::string ChannelId;
    uint32 SenderId = 0;
    std::string SenderUsername;
    std::string SenderName;
    std::string Content;
    SystemTimePoint CreatedAt;
  };

  std::vector<Row> rows = co_await session.OnWorker([channelId, before] {
    std::vector<Row> rows;
    BlogDatabasePreparedStatement* stmt =
        BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_CHAT_MESSAGES);
    stmt->setString(0, channelId);
    stmt->setUInt64(1, before);
    if (PreparedQueryResult result = BlogDatabase.Query(stmt)) {
      do {
        Field* f = result->Fetch();
        rows.push_back(
            {f[0].GetUInt64(), f[1].GetString(), f[2].GetUInt32(), f[3].GetString(),
             f[4].GetString(), f[5].GetString(), f[6].GetDate()});
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
  for (Row const& m : rows) {
    items.push_back(
        Admin::CreateChatMessage(
            builder, m.Id, builder.CreateString(m.ChannelId), m.SenderId,
            builder.CreateString(m.SenderUsername), builder.CreateString(m.SenderName),
            builder.CreateString(m.Content), TimeString(builder, m.CreatedAt)));
  }
  auto cursor = more ? builder.CreateString(std::to_string(rows.back().Id)) : 0;
  builder.Finish(Admin::CreateChatMessagesList(builder, builder.CreateVector(items), cursor));
  request->Respond(builder);
}

// DELETE /api/admin/chat/messages/{id}
Rooftree::Task<void> AdminDeleteChatMessage(Session& session, ApiRequest request) {
  uint64 messageId = request->Get<Admin::ChatMessageRequest>().id().value_or(0);

  struct Deleted {
    std::string ChannelId;  // empty: no such message the moderators may see
    std::vector<uint32> Readers;
  };

  Deleted deleted = co_await session.OnWorker(
      [messageId,
       directory = ChatFileDirectory(sWorld->GetSettings().UploadDirectory)]() -> Deleted {
        BlogDatabasePreparedStatement* stmt =
            BlogDatabase.GetPreparedStatement(BLOG_SEL_CHAT_MESSAGE_META);
        stmt->setUInt64(0, messageId);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row || !IsModeratedChannel(row->Fetch()[0].GetStringView()))
          return {};

        Deleted deleted = {.ChannelId = row->Fetch()[0].GetString()};
        RemoveChatFile(directory, row->Fetch()[4].GetStringView());
        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_CHAT_MESSAGE);
        stmt->setUInt64(0, messageId);
        BlogDatabase.DirectExecute(stmt);
        deleted.Readers = WorkerReadersOf(deleted.ChannelId);
        return deleted;
      });
  if (deleted.ChannelId.empty())
    co_return request->RespondError(status::not_found, "not_found");

  RT_LOG_INFO(
      "blog.admin", "{} deleted the chat message {} of {}", request->GetAuth().User.Username,
      messageId, deleted.ChannelId);
  TellRemoved(deleted.ChannelId, deleted.Readers, messageId);
  RespondRemoved(request, 1);
}

// DELETE /api/admin/chat/channels/{channel}/messages: the square's or a group's every message
Rooftree::Task<void> AdminClearChatChannel(Session& session, ApiRequest request) {
  flatbuffers::String const* value = request->Get<Admin::ChatChannelRequest>().channel();
  std::string channelId = value ? value->str() : std::string();
  if (!IsModeratedChannel(channelId))
    co_return request->RespondError(status::not_found, "not_found");

  struct Cleared {
    bool Found = false;
    uint32 Removed = 0;
    std::vector<uint32> Readers;
  };

  Cleared cleared = co_await session.OnWorker(
      [channelId,
       directory = ChatFileDirectory(sWorld->GetSettings().UploadDirectory)]() -> Cleared {
        Optional<uint32> groupId = GroupIdOf(channelId);
        std::vector<uint32> readers = WorkerReadersOf(channelId);
        if (groupId && readers.empty())
          return {};
        uint32 removed = WorkerCountMessages(channelId);
        WorkerClearChannel(channelId, directory);
        return {true, removed, std::move(readers)};
      });
  if (!cleared.Found)
    co_return request->RespondError(status::not_found, "not_found");

  RT_LOG_INFO(
      "blog.admin", "{} cleared {}: {} messages", request->GetAuth().User.Username, channelId,
      cleared.Removed);
  TellRemoved(channelId, cleared.Readers, {});
  RespondRemoved(request, cleared.Removed);
}

// DELETE /api/admin/chat/groups/{id}: dissolved, its messages with it
Rooftree::Task<void> AdminDeleteChatGroup(Session& session, ApiRequest request) {
  uint32 groupId = request->Get<Admin::IdRequest>().id().value_or(0);

  struct Dissolved {
    uint32 Removed = 0;
    std::vector<uint32> Members;  // none: no such group
  };

  Dissolved dissolved = co_await session.OnWorker(
      [groupId,
       directory = ChatFileDirectory(sWorld->GetSettings().UploadDirectory)]() -> Dissolved {
        std::vector<uint32> members = WorkerGroupMembers(groupId);
        if (members.empty())
          return {};
        uint32 removed = WorkerCountMessages(GroupChannel(groupId));
        WorkerDeleteGroup(groupId, directory);
        return {removed, std::move(members)};
      });
  if (dissolved.Members.empty())
    co_return request->RespondError(status::not_found, "not_found");

  RT_LOG_INFO(
      "blog.admin", "{} dissolved the chat group {} ({} messages)",
      request->GetAuth().User.Username, groupId, dissolved.Removed);
  TellChannelsChanged(dissolved.Members);
  TellRemoved(GroupChannel(groupId), dissolved.Members, {});
  RespondRemoved(request, dissolved.Removed);
}

}  // namespace Blog
