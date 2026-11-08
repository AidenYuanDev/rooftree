/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Comments on a post, a note, a thinking or the guestbook (the target, "post:7", "note:3",
 * "thinking:5", "guestbook"). Two levels like most Chinese sites: top-level comments start threads, every reply sits
 * in its thread and may name whom it answers ("reply @someone"), so a conversation can go on as long
 * as it likes without the page indenting deeper and deeper. Replying to a reply files the new one in
 * the same thread with reply_to_user_id set.
 *
 * Posting needs a login with a confirmed address, an account and an address that aren't banned, and
 * stays within two limits: CommentsPerUser per CommentWindow for an account (wherever it comes from;
 * AntiDOS limits each address on top), and MaxCommentsPerUserPerPost in total under one target.
 *
 * Deleting keeps the row with its text removed, so the replies keep their thread; a deleted
 * top-level comment is shown as such while it has visible replies. Hidden ones (moderation) are only
 * shown to the admin.
 */

#include "Accounts.h"
#include "ApiCall.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "PostAccess.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "Validation.h"
#include <boost/json/array.hpp>
#include <utf8.h>
#include <map>

namespace http = boost::beast::http;

using Rooftree::Server::Session;

namespace Blog
{
namespace
{
constexpr uint32 ThreadsPerPage = 20;
constexpr std::size_t MaxCommentLength = 2000;
// A passage: what is selected, the text around it to find it again, its whole block as it was
constexpr std::size_t MaxQuoteLength = 300;
constexpr std::size_t MaxAnchorContextLength = 32;
constexpr std::size_t MaxAnchorBlockLength = 4000;

// What a comment is on. Id is 0 for the guestbook
struct CommentTarget
{
    std::string Kind;
    uint32 Id = 0;

    std::string ToString() const { return Kind == "guestbook" ? Kind : Rooftree::StringFormat("{}:{}", Kind, Id); }
};

Optional<CommentTarget> ParseTarget(std::string_view text)
{
    if (text == "guestbook")
        return CommentTarget{ "guestbook", 0 };

    std::size_t colon = text.find(':');
    if (colon == std::string_view::npos)
        return {};

    std::string_view kind = text.substr(0, colon);
    Optional<uint32> id = Rooftree::StringTo<uint32>(text.substr(colon + 1));
    if (!id || !*id || (kind != "post" && kind != "note" && kind != "thinking"))
        return {};

    return CommentTarget{ std::string(kind), *id };
}

// A post's passage a comment is on (schema/Reader.fbs CommentAnchor)
struct CommentAnchorRow
{
    std::string Lang;
    uint32 Block = 0;
    std::string Quote;
    std::string Prefix;
    std::string Suffix;
    std::string Text;
    std::string Status = "ok";
};

struct CommentRow
{
    uint32 Id = 0;
    Optional<uint32> ParentId;
    CommentTarget Target;
    uint32 UserId = 0;
    std::string Username;
    std::string DisplayName;
    Optional<uint32> ReplyToUserId;
    std::string ReplyToName;
    std::string Body;
    std::string Status;
    std::string CreatedAt;          // ISO 8601, formatted by PostgreSQL
    Optional<std::string> EditedAt;
    std::string Role;
    std::string Avatar;
    bool AuthorDeleted = false;     // the account was deleted, its comments kept: no name to show
    Optional<uint32> ReplyToId;     // the comment a reply answers
    Optional<CommentAnchorRow> Anchor;
};

// Times as ISO 8601 by PostgreSQL
#define BLOG_COMMENT_COLUMNS "c.id, c.parent_id, c.target_kind, c.target_id, c.user_id, u.username, u.display_name, c.reply_to_user_id, ru.display_name AS reply_to_name, c.body, " \
    "c.status, to_char(c.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS created, to_char(c.edited_at, 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') AS edited, u.role, u.avatar, u.deleted_at IS NOT NULL, " \
    "c.reply_to_id, c.anchor_lang, c.anchor_block, c.anchor_quote, c.anchor_prefix, c.anchor_suffix, c.anchor_text, c.anchor_status"

// Worker: comments matching where (built from numbers only), oldest first
std::vector<CommentRow> WorkerLoadComments(std::string const& where)
{
    std::vector<CommentRow> comments;
    std::string sql = "SELECT " BLOG_COMMENT_COLUMNS " FROM comment c JOIN users u ON u.id = c.user_id "
        "LEFT JOIN users ru ON ru.id = c.reply_to_user_id WHERE " + where + " ORDER BY c.id";
    QueryResult result = BlogDatabase.Query(sql.c_str());
    if (!result)
        return comments;

    do
    {
        Field* fields = result->Fetch();
        CommentRow comment;
        comment.Id = fields[0].GetUInt32();
        comment.ParentId = fields[1].GetUInt32OrNull();
        comment.Target = { fields[2].GetString(), fields[3].GetUInt32() };
        comment.UserId = fields[4].GetUInt32();
        comment.Username = fields[5].GetString();
        comment.DisplayName = fields[6].GetString();
        comment.ReplyToUserId = fields[7].GetUInt32OrNull();
        comment.ReplyToName = fields[8].GetStringOrNull().value_or("");
        comment.Body = fields[9].GetString();
        comment.Status = fields[10].GetString();
        comment.CreatedAt = fields[11].GetString();
        comment.EditedAt = fields[12].GetStringOrNull();
        comment.Role = fields[13].GetString();
        comment.Avatar = fields[14].GetStringOrNull().value_or("");
        comment.AuthorDeleted = fields[15].GetInt64() != 0;
        comment.ReplyToId = fields[16].GetUInt32OrNull();
        if (Optional<std::string> lang = fields[17].GetStringOrNull())
            comment.Anchor = CommentAnchorRow{ .Lang = std::move(*lang), .Block = fields[18].GetUInt32OrNull().value_or(0),
                .Quote = fields[19].GetStringOrNull().value_or(""), .Prefix = fields[20].GetStringOrNull().value_or(""),
                .Suffix = fields[21].GetStringOrNull().value_or(""), .Text = fields[22].GetStringOrNull().value_or(""),
                .Status = fields[23].GetString() };
        comments.push_back(std::move(comment));
    } while (result->NextRow());

    return comments;
}

Optional<CommentRow> WorkerLoadComment(uint32 id)
{
    std::vector<CommentRow> comments = WorkerLoadComments(Rooftree::StringFormat("c.id = {}", id));
    if (comments.empty())
        return {};
    return std::move(comments.front());
}

std::string JoinIds(std::vector<uint32> const& ids)
{
    std::string list;
    for (uint32 id : ids)
    {
        if (!list.empty())
            list += ',';
        list += std::to_string(id);
    }
    return list;
}

// Someone to mail about a reply to their comment
struct ReplyNotice
{
    UserRow Recipient;
    std::string Lang;       // of the page linked: the recipient's when the page has it
    std::string Title;      // of the post or note, empty for a thinking or the guestbook
    std::string Path;       // "/zh/posts/hello", without the comment's anchor
};

// Worker: the mail's page for a comment on target, in lang if it has that language
Optional<ReplyNotice> WorkerReplyPage(CommentTarget const& target, std::string const& lang)
{
    ReplyNotice notice;
    notice.Lang = lang;
    if (target.Kind == "post" || target.Kind == "note")
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(target.Kind == "post" ? BLOG_SEL_POST_TITLES : BLOG_SEL_NOTE_TITLES);
        stmt->setUInt32(0, target.Id);
        PreparedQueryResult rows = BlogDatabase.Query(stmt);
        if (!rows)
            return {};
        std::map<std::string, std::string> titles;
        std::string slug;
        do
        {
            Field* fields = rows->Fetch();
            titles[fields[0].GetString()] = fields[1].GetString();
            slug = fields[2].GetString();
        } while (rows->NextRow());
        // Each language's site has only its own pages
        if (!titles.contains(lang))
            notice.Lang = titles.begin()->first;
        notice.Title = titles[notice.Lang];
        notice.Path = target.Kind == "post" ? "/" + notice.Lang + "/posts/" + slug : Rooftree::StringFormat("/{}/notes/{}", notice.Lang, target.Id);
    }
    else if (target.Kind == "thinking")
        notice.Path = "/" + lang + "/thinking";
    else
        notice.Path = "/" + lang + "/guestbook";
    return notice;
}

// A reply to recipient's comment, by replier: what they wrote, a link to it, how to turn these off
Rooftree::Server::MailMessage ReplyMail(ReplyNotice const& notice, CommentRow const& reply)
{
    BlogSettings const& settings = sWorld->GetSettings();
    std::string site = sWorld->GetSiteName(notice.Lang);
    std::string link = Rooftree::StringFormat("{}{}#comment-{}", settings.PublicOrigin, notice.Path, reply.Id);
    std::string account = Rooftree::StringFormat("{}/{}/account", settings.PublicOrigin, notice.Lang);
    // The first 300 characters, quoted
    std::string excerpt;
    std::size_t characters = 0;
    for (std::size_t i = 0; i < reply.Body.size() && characters < 300; ++characters)
    {
        std::size_t length = 1;
        unsigned char c = reply.Body[i];
        if (c >= 0xf0) length = 4; else if (c >= 0xe0) length = 3; else if (c >= 0xc0) length = 2;
        excerpt.append(reply.Body, i, length);
        i += length;
    }
    if (excerpt.size() < reply.Body.size())
        excerpt += "…";
    std::string quoted;
    for (std::string_view line : Rooftree::Tokenize(excerpt, '\n', true))
        quoted += "> " + std::string(line) + "\n";

    UserRow const& to = notice.Recipient;
    if (notice.Lang == "en")
    {
        std::string where = !notice.Title.empty() ? "under “" + notice.Title + "”" : reply.Target.Kind == "thinking" ? "under a thought" : "in the guestbook";
        return { .To = to.Email, .Subject = reply.DisplayName + " replied to you on " + site,
            .TextBody = Rooftree::StringFormat("Hi {},\n\n{} replied to your comment {}:\n\n{}\nRead it and answer:\n{}\n\n--\n"
                "You get these as {} on {}. Turn them off in your account: {}\n", to.DisplayName, reply.DisplayName, where, quoted, link,
                to.Username, site, account) };
    }
    std::string where = !notice.Title.empty() ? "在《" + notice.Title + "》下" : reply.Target.Kind == "thinking" ? "在一句话下" : "在留言板";
    return { .To = to.Email, .Subject = reply.DisplayName + " 在" + site + "回复了你",
        .TextBody = Rooftree::StringFormat("{}，你好：\n\n{} {}回复了你的评论：\n\n{}\n去看看，或者回复 TA：\n{}\n\n——\n"
            "你在{}的账号是 {}。不想收到这类邮件，可以在账号设置里关掉：{}\n", to.DisplayName, reply.DisplayName, where, quoted, link,
            site, to.Username, account) };
}

// A reader's comment; a thread's root has its replies
flatbuffers::Offset<Reader::Comment> ReaderCommentTable(flatbuffers::FlatBufferBuilder& builder, CommentRow const& comment,
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Reader::Comment>>> replies = 0)
{
    bool deleted = comment.Status == "deleted";
    flatbuffers::Offset<Reader::CommentAuthor> author;
    // A deleted comment keeps its place, not its author
    if (!deleted)
        author = Reader::CreateCommentAuthor(builder, comment.UserId, builder.CreateString(comment.Username),
            builder.CreateString(comment.DisplayName), builder.CreateString(comment.Role),
            comment.Avatar.empty() ? 0 : builder.CreateString("/uploads/" + comment.Avatar), comment.AuthorDeleted);
    auto replyTo = comment.ReplyToUserId ? Reader::CreateCommentReplyTo(builder, *comment.ReplyToUserId, builder.CreateString(comment.ReplyToName)) : 0;
    // Its block as it was only once the post no longer has it: the page finds the rest in the post
    flatbuffers::Offset<Reader::CommentAnchor> anchor;
    if (CommentAnchorRow const* passage = comment.Anchor ? &*comment.Anchor : nullptr)
        anchor = Reader::CreateCommentAnchor(builder, builder.CreateString(passage->Lang), passage->Block, builder.CreateString(passage->Quote),
            builder.CreateString(passage->Prefix), builder.CreateString(passage->Suffix),
            passage->Status == "ok" ? 0 : builder.CreateString(passage->Text), builder.CreateString(passage->Status));
    return Reader::CreateComment(builder, comment.Id, builder.CreateString(comment.Target.ToString()),
        comment.ParentId ? flatbuffers::Optional<uint32>(*comment.ParentId) : flatbuffers::nullopt, builder.CreateString(comment.Status),
        builder.CreateString(deleted ? std::string() : comment.Body), builder.CreateString(comment.CreatedAt),
        comment.EditedAt ? builder.CreateString(*comment.EditedAt) : 0, author, replyTo, replies,
        comment.ReplyToId ? flatbuffers::Optional<uint32>(*comment.ReplyToId) : flatbuffers::nullopt, anchor);
}

// A passage as sent: none when the request has none
struct AnchorInput
{
    std::string Lang;
    uint32 Block = 0;
    std::string Quote;
    std::string Prefix;
    std::string Suffix;
    std::string Text;
};

// Taken as the page sent it, not trimmed: the text around a selection begins and ends where it does
std::string AnchorText(TableFields& fields, std::string_view key, flatbuffers::String const* value, std::size_t minLength, std::size_t maxLength)
{
    std::string_view text = value ? value->string_view() : std::string_view();
    if (!utf8::is_valid(text.begin(), text.end()) || text.find('\0') != std::string_view::npos)
        fields.AddError(key, "invalid");
    else if (CountCharacters(text) < minLength)
        fields.AddError(key, text.empty() ? "required" : "too_short");
    else if (CountCharacters(text) > maxLength)
        fields.AddError(key, "too_long");
    return std::string(text);
}

Optional<AnchorInput> ReadAnchor(TableFields& fields, Reader::CommentAnchor const* in)
{
    if (!in)
        return {};

    AnchorInput anchor;
    anchor.Lang = fields.Choice("anchor.lang", in->lang(), { "zh", "en" });
    anchor.Block = in->block();
    anchor.Quote = AnchorText(fields, "anchor.quote", in->quote(), 1, MaxQuoteLength);
    anchor.Prefix = AnchorText(fields, "anchor.prefix", in->prefix(), 0, MaxAnchorContextLength);
    anchor.Suffix = AnchorText(fields, "anchor.suffix", in->suffix(), 0, MaxAnchorContextLength);
    anchor.Text = AnchorText(fields, "anchor.text", in->text(), 1, MaxAnchorBlockLength);
    // Its quote is in its block
    if (fields.IsValid() && anchor.Text.find(anchor.Quote) == std::string::npos)
        fields.AddError("anchor.quote", "not_in_text");
    return anchor;
}

// Worker: the passage's post has its language, and the revision it is on
Optional<uint32> WorkerAnchorRevision(uint32 postId, std::string const& lang, bool& hasLang)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_HAS_LANG);
    stmt->setUInt32(0, postId);
    stmt->setString(1, lang);
    hasLang = BlogDatabase.Query(stmt) != nullptr;
    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_LATEST_REVISION);
    stmt->setUInt32(0, postId);
    stmt->setString(1, lang);
    if (PreparedQueryResult row = BlogDatabase.Query(stmt))
        return row->Fetch()[0].GetUInt32();
    return {};
}

void RespondComment(ApiRequest const& request, CommentRow const& comment, http::status status = http::status::ok)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(ReaderCommentTable(builder, comment));
    request->Respond(builder, status);
}

// A comment as the admin's table
flatbuffers::Offset<Admin::Comment> CommentTable(flatbuffers::FlatBufferBuilder& builder, CommentRow const& comment)
{
    bool deleted = comment.Status == "deleted";
    flatbuffers::Offset<Admin::CommentAuthor> author;
    // A deleted comment keeps its place, not its author
    if (!deleted)
        author = Admin::CreateCommentAuthor(builder, comment.UserId, builder.CreateString(comment.Username), builder.CreateString(comment.DisplayName),
            builder.CreateString(comment.Role), comment.Avatar.empty() ? 0 : builder.CreateString("/uploads/" + comment.Avatar), comment.AuthorDeleted);
    auto replyTo = comment.ReplyToUserId ? Admin::CreateCommentReplyTo(builder, *comment.ReplyToUserId, builder.CreateString(comment.ReplyToName)) : 0;
    flatbuffers::Offset<Admin::CommentPassage> passage;
    if (comment.Anchor)
        passage = Admin::CreateCommentPassage(builder, builder.CreateString(comment.Anchor->Lang), builder.CreateString(comment.Anchor->Quote),
            builder.CreateString(comment.Anchor->Text), builder.CreateString(comment.Anchor->Status));
    return Admin::CreateComment(builder, comment.Id, builder.CreateString(comment.Target.ToString()),
        comment.ParentId ? flatbuffers::Optional<uint32>(*comment.ParentId) : flatbuffers::nullopt, builder.CreateString(comment.Status),
        builder.CreateString(deleted ? std::string() : comment.Body), builder.CreateString(comment.CreatedAt),
        comment.EditedAt ? builder.CreateString(*comment.EditedAt) : 0, author, replyTo, passage);
}

// When the author may still edit a comment created at createdAt (ISO 8601 UTC)
bool IsWithinEditWindow(std::string const& createdAt, Seconds window)
{
    std::istringstream in(createdAt);
    std::chrono::sys_seconds created;
    in >> std::chrono::parse("%Y-%m-%dT%H:%M:%SZ", created);
    return !in.fail() && SystemTimePoint::clock::now() < created + window;
}

// target=note:3, or the older postId=7
Optional<CommentTarget> GetTarget(Reader::CommentsListRequest const& in)
{
    if (in.target())
        return ParseTarget(in.target()->string_view());
    if (in.postId() && *in.postId())
        return CommentTarget{ "post", *in.postId() };
    return {};
}

// Worker: whether target takes comments; for posts, notes and thinking, a published one with comments on
enum class TargetState
{
    Open,
    NotFound,
    Closed
};

TargetState WorkerCheckTarget(CommentTarget const& target)
{
    if (target.Kind == "guestbook")
        return TargetState::Open;

    BlogDatabaseStatements statement = target.Kind == "post" ? BLOG_SEL_POST_FOR_COMMENT
        : target.Kind == "note" ? BLOG_SEL_NOTE_FOR_COMMENT : BLOG_SEL_THINKING_FOR_COMMENT;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(statement);
    stmt->setUInt32(0, target.Id);
    PreparedQueryResult row = BlogDatabase.Query(stmt);
    if (!row || row->Fetch()[0].GetString() != "published")
        return TargetState::NotFound;
    return row->Fetch()[1].GetBool() ? TargetState::Open : TargetState::Closed;
}
}

namespace
{
// A CommentsList: the threads in that order, each with its replies (comments has both), a cursor
// after the last when there are more
std::string ThreadsAnswer(std::vector<uint32> const& threadIds, std::vector<CommentRow> const& comments, bool hasMore)
{
    std::map<uint32, std::vector<CommentRow const*>> replies;
    std::map<uint32, CommentRow const*> roots;
    for (CommentRow const& comment : comments)
    {
        if (comment.ParentId)
            replies[*comment.ParentId].push_back(&comment);
        else
            roots[comment.Id] = &comment;
    }

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::Comment>> threads;
    for (uint32 id : threadIds)
    {
        auto root = roots.find(id);
        if (root == roots.end())
            continue;

        std::vector<flatbuffers::Offset<Reader::Comment>> answers;
        for (CommentRow const* reply : replies[id])
            answers.push_back(ReaderCommentTable(builder, *reply));
        threads.push_back(ReaderCommentTable(builder, *root->second, builder.CreateVector(answers)));
    }

    std::string nextCursor = hasMore ? std::to_string(threadIds.back()) : std::string();
    builder.Finish(Reader::CreateCommentsList(builder, builder.CreateVector(threads), nextCursor.empty() ? 0 : builder.CreateString(nextCursor)));
    return Bytes(builder);
}

// Worker: the comments of these threads, the replies a reader sees (a moderator's: hidden ones too)
std::vector<CommentRow> WorkerLoadThreads(std::vector<uint32> const& threads, bool moderator)
{
    if (threads.empty())
        return {};
    std::string ids = JoinIds(threads);
    std::string replyStatus = moderator ? "c.status IN ('visible', 'hidden')" : "c.status = 'visible'";
    return WorkerLoadComments(Rooftree::StringFormat("c.id IN ({}) OR (c.parent_id IN ({}) AND {})", ids, ids, replyStatus));
}
}

namespace
{
struct CommentPage
{
    std::vector<CommentRow> Comments;
    std::vector<uint32> Threads;
    bool HasMore = false;
};

Rooftree::Task<void> LoadCommentPage(Session& session, ApiRequest request, CommentTarget target, uint32 before, bool admin, std::string key, uint64 generation)
{
    CommentPage page = co_await session.OnWorker([&target, before, admin]
    {
        CommentPage result;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_COMMENT_THREADS);
        stmt->setString(0, target.Kind);
        stmt->setUInt32(1, target.Id);
        stmt->setUInt8(2, admin ? 1 : 0);
        stmt->setUInt32(3, before);
        stmt->setUInt32(4, ThreadsPerPage + 1);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
                result.Threads.push_back(rows->Fetch()[0].GetUInt32());
            while (rows->NextRow());
        }

        if (result.Threads.size() > ThreadsPerPage)
        {
            result.Threads.resize(ThreadsPerPage);
            result.HasMore = true;
        }

        result.Comments = WorkerLoadThreads(result.Threads, admin);
        return result;
    });

    std::string answer = ThreadsAnswer(page.Threads, page.Comments, page.HasMore);
    if (!admin)
        sWorld->GetReadCache().Store(std::move(key), answer, generation);
    request->Respond(std::move(answer));
}
}

Rooftree::Task<void> ListComments(Session& session, ApiRequest request)
{
    Reader::CommentsListRequest const& in = request->Get<Reader::CommentsListRequest>();
    Optional<CommentTarget> target = GetTarget(in);
    if (!target)
        co_return RespondValidation(request, { { "target", "required" } });

    uint32 before = std::numeric_limits<uint32>::max();
    if (in.cursor() && in.cursor()->size())
    {
        Optional<uint32> value = Rooftree::StringTo<uint32>(in.cursor()->string_view());
        if (!value)
            co_return request->RespondError(http::status::bad_request, "invalid_cursor");
        before = *value;
    }

    bool admin = request->GetLogin() && request->GetLogin()->User.CanModerate();
    // What every reader sees is kept in memory (ReadCache.h); a moderator's has the hidden ones too
    std::string key = Rooftree::StringFormat("comments|{}|{}|{}", target->Kind, target->Id, before);
    ReadCache& cache = sWorld->GetReadCache();
    if (!admin)
        if (std::string const* answer = cache.Find(key))
            co_return request->Respond(*answer);

    session.Spawn(LoadCommentPage(session, request, *target, before, admin, std::move(key), cache.GetGeneration()));
}

Rooftree::Task<void> ListAnchoredComments(Session& session, ApiRequest request)
{
    Reader::CommentsAnchoredRequest const& in = request->Get<Reader::CommentsAnchoredRequest>();
    TableFields fields(request->GetErrors());
    Optional<CommentTarget> target = ParseTarget(fields.Text("target", in.target(), 1, 30));
    std::string lang = fields.Choice("lang", in.lang(), { "zh", "en" });
    if (!target || target->Kind != "post")
        fields.AddError("target", "invalid");
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    bool moderator = request->GetLogin() && request->GetLogin()->User.CanModerate();
    std::string key = Rooftree::StringFormat("anchored|{}|{}", target->Id, lang);
    ReadCache& cache = sWorld->GetReadCache();
    if (!moderator)
        if (std::string const* answer = cache.Find(key))
            co_return request->Respond(*answer);

    struct Result
    {
        std::vector<uint32> Threads;
        std::vector<CommentRow> Comments;
    };

    auto generation = cache.GetGeneration();
    Result result = co_await session.OnWorker([postId = target->Id, lang, moderator]
    {
        Result result;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ANCHORED_THREADS);
        stmt->setUInt32(0, postId);
        stmt->setString(1, lang);
        stmt->setUInt8(2, moderator ? 1 : 0);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
                result.Threads.push_back(rows->Fetch()[0].GetUInt32());
            while (rows->NextRow());
        }
        result.Comments = WorkerLoadThreads(result.Threads, moderator);
        return result;
    });
    std::string answer = ThreadsAnswer(result.Threads, result.Comments, false);
    if (!moderator)
        sWorld->GetReadCache().Store(std::move(key), answer, generation);
    request->Respond(std::move(answer));
}

Rooftree::Task<void> CreateComment(Session& session, ApiRequest request)
{
    AuthInfo const& auth = request->GetAuth();
    if (!auth.User.EmailVerified)
        co_return request->RespondError(http::status::forbidden, "email_unverified");
    if (auth.User.IsDenied(DenyComment))
        co_return request->RespondError(http::status::forbidden, "not_allowed");

    if (sWorld->IsIpBanned(request->GetClientAddress()))
    {
        RT_LOG_INFO("blog.comments", "{} ({}) can't comment, the address is banned", auth.User.Username, request->GetClientAddress().to_string());
        co_return request->RespondError(http::status::forbidden, "ip_banned");
    }

    Reader::CommentInput const& in = request->Get<Reader::CommentInput>();
    TableFields fields(request->GetErrors());
    // "target": "note:3", or the older "postId": 7
    Optional<CommentTarget> commentTarget;
    if (in.target())
        commentTarget = ParseTarget(fields.Text("target", in.target(), 1, 30));
    else if (in.postId() && *in.postId() > 0 && *in.postId() <= std::numeric_limits<uint32>::max())
        commentTarget = CommentTarget{ "post", uint32(*in.postId()) };
    if (!commentTarget)
        fields.AddError("target", "required");
    std::string text = fields.Text("body", in.body(), 1, MaxCommentLength, true, true);
    if (in.replyTo() && (*in.replyTo() <= 0 || *in.replyTo() > std::numeric_limits<uint32>::max()))
        fields.AddError("replyTo", "invalid");
    // A passage only of a post
    Optional<AnchorInput> anchor = ReadAnchor(fields, in.anchor());
    if (anchor && commentTarget && commentTarget->Kind != "post")
        fields.AddError("anchor", "not_allowed");
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    if (!sWorld->GetCommentLimiter().Allow(auth.User.Id))
        co_return request->RespondError(http::status::too_many_requests, "too_many_comments");

    enum class CommentOutcome
    {
        Ok,
        PostNotFound,
        Closed,
        LimitReached,
        InvalidReply,
        NoSuchLang
    };

    struct Result
    {
        CommentOutcome Outcome = CommentOutcome::Ok;
        Optional<CommentRow> Comment;
        Optional<ReplyNotice> Notice;   // the replied comment's author, to mail
    };

    uint32 userId = auth.User.Id;
    uint32 maxPerPost = sWorld->GetSettings().MaxCommentsPerUserPerPost;
    std::string ip = request->GetClientAddress().to_string();
    Optional<uint32> target = in.replyTo() ? Optional<uint32>(uint32(*in.replyTo())) : Optional<uint32>();
    Result result = co_await session.OnWorker([=, on = *commentTarget]
    {
        Result result;
        switch (WorkerCheckTarget(on))
        {
            case TargetState::NotFound:
                result.Outcome = CommentOutcome::PostNotFound;
                return result;
            case TargetState::Closed:
                result.Outcome = CommentOutcome::Closed;
                return result;
            case TargetState::Open:
                break;
        }

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_TARGET_COMMENT_COUNT);
        stmt->setUInt32(0, userId);
        stmt->setString(1, on.Kind);
        stmt->setUInt32(2, on.Id);
        PreparedQueryResult count = BlogDatabase.Query(stmt);
        if (count && count->Fetch()[0].GetInt64() >= int64(maxPerPost))
        {
            result.Outcome = CommentOutcome::LimitReached;
            return result;
        }

        // The thread is the replied comment's, or the replied comment itself when it starts one
        Optional<uint32> parentId;
        Optional<uint32> replyToUserId;
        if (target)
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_COMMENT);
            stmt->setUInt32(0, *target);
            PreparedQueryResult replied = BlogDatabase.Query(stmt);
            Field* fields = replied ? replied->Fetch() : nullptr;
            if (!fields || fields[1].GetString() != on.Kind || fields[2].GetUInt32() != on.Id || fields[5].GetString() != "visible")
            {
                result.Outcome = CommentOutcome::InvalidReply;
                return result;
            }

            Optional<uint32> repliedParent = fields[4].GetUInt32OrNull();
            parentId = repliedParent ? *repliedParent : *target;
            if (repliedParent)
                replyToUserId = fields[3].GetUInt32();

            // Its author hears of it: not one's own, an address that works, mails not turned off
            Optional<UserRow> recipient = FindUserById(fields[3].GetUInt32());
            if (recipient && recipient->Id != userId && !recipient->Deleted && recipient->EmailVerified && recipient->ReplyMails)
            {
                result.Notice = WorkerReplyPage(on, recipient->Locale);
                if (result.Notice)
                    result.Notice->Recipient = std::move(*recipient);
            }
        }

        // The passage is in a language the post has
        Optional<uint32> revision;
        if (anchor)
        {
            bool hasLang = false;
            revision = WorkerAnchorRevision(on.Id, anchor->Lang, hasLang);
            if (!hasLang)
            {
                result.Outcome = CommentOutcome::NoSuchLang;
                return result;
            }
        }

        stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_COMMENT);
        for (uint8 column = 0; std::string_view kind : { "post", "note", "thinking" })
        {
            if (on.Kind == kind)
                stmt->setUInt32(column, on.Id);
            else
                stmt->setNull(column);
            ++column;
        }
        stmt->setString(3, on.Kind);
        stmt->setUInt32(4, on.Id);
        stmt->setUInt32(5, userId);
        if (parentId)
            stmt->setUInt32(6, *parentId);
        else
            stmt->setNull(6);
        if (replyToUserId)
            stmt->setUInt32(7, *replyToUserId);
        else
            stmt->setNull(7);
        stmt->setString(8, text);
        stmt->setString(9, ip);
        if (target)
            stmt->setUInt32(10, *target);
        else
            stmt->setNull(10);
        if (anchor)
        {
            stmt->setString(11, anchor->Lang);
            stmt->setUInt32(12, anchor->Block);
            stmt->setString(13, anchor->Quote);
            stmt->setString(14, anchor->Prefix);
            stmt->setString(15, anchor->Suffix);
            stmt->setString(16, anchor->Text);
            if (revision)
                stmt->setUInt32(17, *revision);
            else
                stmt->setNull(17);
        }
        else
            for (uint8 column = 11; column <= 17; ++column)
                stmt->setNull(column);
        BlogDatabase.DirectExecute(stmt);

        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_LAST_USER_COMMENT);
        stmt->setUInt32(0, userId);
        stmt->setString(1, on.Kind);
        stmt->setUInt32(2, on.Id);
        if (PreparedQueryResult inserted = BlogDatabase.Query(stmt))
            result.Comment = WorkerLoadComment(inserted->Fetch()[0].GetUInt32());
        return result;
    });
    switch (result.Outcome)
    {
        case CommentOutcome::PostNotFound:
            co_return RespondNotFound(request);
        case CommentOutcome::Closed:
            co_return request->RespondError(http::status::forbidden, "comments_closed");
        case CommentOutcome::LimitReached:
            co_return request->RespondError(http::status::forbidden, "comment_limit_reached");
        case CommentOutcome::InvalidReply:
            co_return RespondValidation(request, { { "replyTo", "invalid" } });
        case CommentOutcome::NoSuchLang:
            co_return RespondValidation(request, { { "anchor.lang", "invalid" } });
        case CommentOutcome::Ok:
            break;
    }

    if (!result.Comment)
        co_return RespondInternalError(request);

    RT_LOG_INFO("blog.comments", "{} commented on {} ({})", result.Comment->Username, result.Comment->Target.ToString(), result.Comment->Id);
    sWorld->RequestCommentCounts();
    sWorld->AnnounceChanged(result.Comment->Target.ToString(), "comments");
    if (result.Comment->Target.Kind == "thinking")
        sWorld->AnnounceChanged("thinking", "content");
    if (result.Notice)
    {
        if (sWorld->GetReplyMailLimiter().Allow(result.Notice->Recipient.Id))
            sWorld->SendMail(session, ReplyMail(*result.Notice, *result.Comment));
        else
            RT_LOG_INFO("blog.comments", "No reply mail to {}: enough of them this hour", result.Notice->Recipient.Username);
    }
    RespondComment(request, *result.Comment, http::status::created);
}

Rooftree::Task<void> UpdateComment(Session& session, ApiRequest request)
{
    AuthInfo const& auth = request->GetAuth();
    if (auth.User.IsDenied(DenyComment))
        co_return request->RespondError(http::status::forbidden, "not_allowed");

    Reader::CommentsUpdateRequest const& in = request->Get<Reader::CommentsUpdateRequest>();
    if (!in.id())
        co_return RespondNotFound(request);
    uint32 id = *in.id();
    TableFields fields(request->GetErrors());
    // Its text, its passage or both: without a passage the text is what is sent
    Optional<AnchorInput> anchor = ReadAnchor(fields, in.anchor());
    std::string text;
    if (in.body() || !anchor)
        text = fields.Text("body", in.body(), 1, MaxCommentLength, true, true);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    struct Result
    {
        Optional<CommentRow> Comment;
        std::string_view Error;
    };

    uint32 userId = auth.User.Id;
    bool moderator = auth.User.CanModerate();
    Seconds window = sWorld->GetSettings().CommentEditWindow;
    Result result = co_await session.OnWorker([=]
    {
        Result result;
        result.Comment = WorkerLoadComment(id);
        if (!result.Comment || result.Comment->Status == "deleted")
        {
            result.Error = "not_found";
            return result;
        }

        bool own = result.Comment->UserId == userId;
        bool editable = own && IsWithinEditWindow(result.Comment->CreatedAt, window);
        if (!text.empty() && !editable)
            result.Error = own ? "edit_window_passed" : "forbidden";
        else if (anchor)
        {
            // Its passage later too: its author once the post lost it, a moderator any time
            bool lost = result.Comment->Anchor && result.Comment->Anchor->Status != "ok";
            if (result.Comment->Target.Kind != "post")
                result.Error = "anchor_not_allowed";
            else if (!editable && !(own && lost) && !moderator)
                result.Error = own ? "edit_window_passed" : "forbidden";
        }
        if (!result.Error.empty())
            return result;

        if (anchor)
        {
            bool hasLang = false;
            Optional<uint32> revision = WorkerAnchorRevision(result.Comment->Target.Id, anchor->Lang, hasLang);
            if (!hasLang)
            {
                result.Error = "anchor_lang";
                return result;
            }

            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_COMMENT_ANCHOR);
            stmt->setString(0, anchor->Lang);
            stmt->setUInt32(1, anchor->Block);
            stmt->setString(2, anchor->Quote);
            stmt->setString(3, anchor->Prefix);
            stmt->setString(4, anchor->Suffix);
            stmt->setString(5, anchor->Text);
            if (revision)
                stmt->setUInt32(6, *revision);
            else
                stmt->setNull(6);
            stmt->setUInt32(7, id);
            BlogDatabase.DirectExecute(stmt);
        }

        if (!text.empty())
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_COMMENT_BODY);
            stmt->setString(0, text);
            stmt->setUInt32(1, id);
            BlogDatabase.DirectExecute(stmt);
        }
        result.Comment = WorkerLoadComment(id);
        return result;
    });
    if (result.Error == "not_found")
        co_return RespondNotFound(request);
    if (result.Error == "anchor_not_allowed")
        co_return RespondValidation(request, { { "anchor", "not_allowed" } });
    if (result.Error == "anchor_lang")
        co_return RespondValidation(request, { { "anchor.lang", "invalid" } });
    if (!result.Error.empty())
        co_return request->RespondError(http::status::forbidden, result.Error);
    if (!result.Comment)
        co_return RespondInternalError(request);

    // Its text and passage are in the lists kept in memory (ReadCache.h); its count didn't change
    sWorld->GetReadCache().Invalidate();
    sWorld->AnnounceChanged(result.Comment->Target.ToString(), "comments");
    RespondComment(request, *result.Comment);
}

// Worker: moderators, and a post's owner the comments on it
bool WorkerMayModerate(CommentRow const& comment, UserRow const& user)
{
    if (user.CanModerate())
        return true;
    if (comment.Target.Kind != "post")
        return false;
    Optional<PostAccess> access = WorkerPostAccess(comment.Target.Id, user);
    return access && access->Owner;
}

// Banned users may still take their comments down
Rooftree::Task<void> DeleteComment(Session& session, ApiRequest request)
{
    flatbuffers::Optional<uint32> id = request->Get<Reader::IdRequest>().id();
    if (!id)
        co_return RespondNotFound(request);

    UserRow const& user = request->GetAuth().User;
    // What went wrong, else where it was ("post:7")
    struct Result
    {
        std::string_view Error;
        std::string Target;
    };
    Result result = co_await session.OnWorker([user, id = *id]() -> Result
    {
        Optional<CommentRow> comment = WorkerLoadComment(id);
        if (!comment || comment->Status == "deleted")
            return { .Error = "not_found" };
        if (comment->UserId != user.Id && !WorkerMayModerate(*comment, user))
            return { .Error = "forbidden" };

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_COMMENT_DELETED);
        stmt->setUInt32(0, id);
        BlogDatabase.DirectExecute(stmt);
        return { .Target = comment->Target.ToString() };
    });
    if (result.Error == "not_found")
        co_return RespondNotFound(request);
    if (!result.Error.empty())
        co_return request->RespondError(http::status::forbidden, result.Error);

    sWorld->RequestCommentCounts();
    if (result.Target.starts_with("thinking:"))
        sWorld->AnnounceChanged("thinking", "content");
    sWorld->AnnounceChanged(std::move(result.Target), "comments");
    RespondOk(request);
}

// Admin moderation, here for CommentRow

Rooftree::Task<void> AdminListComments(Session& session, ApiRequest request)
{
    Admin::CommentsListRequest const& in = request->Get<Admin::CommentsListRequest>();
    std::string status = in.status() ? in.status()->str() : std::string();
    if (!status.empty() && status != "visible" && status != "hidden" && status != "deleted")
        co_return RespondValidation(request, { { "status", "invalid" } });
    std::string passage = in.passage() ? in.passage()->str() : std::string();
    if (!passage.empty() && passage != "any" && passage != "orphaned")
        co_return RespondValidation(request, { { "passage", "invalid" } });

    uint32 before = (in.cursor() ? Rooftree::StringTo<uint32>(in.cursor()->string_view()) : Optional<uint32>()).value_or(std::numeric_limits<uint32>::max());
    std::string payload = co_await session.OnWorker([status, passage, before]
    {
        // Built here: the answer is all the worker's, the next page's cursor the 51st row
        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<Admin::CommentsListItem>> comments;
        uint32 lastId = 0;
        bool more = false;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_COMMENTS);
        stmt->setString(0, status);
        stmt->setString(1, status);
        for (uint8 column = 2; column <= 4; ++column)
            stmt->setString(column, passage);
        stmt->setUInt32(5, before);
        stmt->setUInt32(6, 51);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                if (comments.size() == 50)
                {
                    more = true;
                    break;
                }

                Field* fields = rows->Fetch();
                Optional<SystemTimePoint> bannedUntil = fields[12].GetDateOrNull();
                CommentTarget target{ fields[1].GetString(), fields[2].GetUInt32() };
                auto author = Admin::CreateCommentsListAuthor(builder, fields[4].GetUInt32(), builder.CreateString(fields[5].GetString()),
                    builder.CreateString(fields[6].GetString()), bannedUntil && *bannedUntil > SystemTimePoint::clock::now());
                // Where it is shown: a post by its slug, the others by number
                Optional<std::string> postSlug = fields[3].GetStringOrNull();
                lastId = fields[0].GetUInt32();
                flatbuffers::Offset<Admin::CommentPassage> on;
                if (Optional<std::string> lang = fields[13].GetStringOrNull())
                    on = Admin::CreateCommentPassage(builder, builder.CreateString(*lang), builder.CreateString(fields[14].GetStringOrNull().value_or("")),
                        builder.CreateString(fields[15].GetStringOrNull().value_or("")), builder.CreateString(fields[16].GetString()));
                comments.push_back(Admin::CreateCommentsListItem(builder, lastId, builder.CreateString(target.ToString()),
                    postSlug ? builder.CreateString(*postSlug) : 0, author, builder.CreateString(fields[7].GetString()),
                    builder.CreateString(fields[8].GetString()), TimeString(builder, fields[9].GetDate()), builder.CreateString(fields[10].GetString()),
                    !fields[11].IsNull(), on));
            } while (rows->NextRow());
        }
        auto list = builder.CreateVector(comments);
        builder.Finish(Admin::CreateCommentsList(builder, list, more ? builder.CreateString(std::to_string(lastId)) : 0));
        return Bytes(builder);
    });
    request->Respond(std::move(payload));
}

Rooftree::Task<void> AdminModerateComment(Session& session, ApiRequest request)
{
    Admin::CommentsModerateRequest const& in = request->Get<Admin::CommentsModerateRequest>();
    if (!in.id())
        co_return RespondNotFound(request);

    uint32 id = *in.id();
    TableFields fields(request->GetErrors());
    // Its status, its lost passage left so, or both
    std::string status = fields.Choice("status", in.status(), { "visible", "hidden" }, !in.passage());
    std::string passage = fields.Choice("passage", in.passage(), { "kept" }, false);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    // Not allowed: the comment, none
    std::pair<bool, Optional<CommentRow>> result = co_await session.OnWorker([id, status, passage, user = request->GetAuth().User]() -> std::pair<bool, Optional<CommentRow>>
    {
        Optional<CommentRow> current = WorkerLoadComment(id);
        if (!current)
            return { true, {} };
        if (!WorkerMayModerate(*current, user))
            return { false, {} };
        if (!status.empty())
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_COMMENT_STATUS);
            stmt->setString(0, status);
            stmt->setUInt32(1, id);
            BlogDatabase.DirectExecute(stmt);
        }
        if (!passage.empty())
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_COMMENT_PASSAGE_KEPT);
            stmt->setUInt32(0, id);
            BlogDatabase.DirectExecute(stmt);
        }
        return { true, WorkerLoadComment(id) };
    });
    if (!result.first)
        co_return request->RespondError(http::status::forbidden, "forbidden");
    Optional<CommentRow>& comment = result.second;
    if (!comment)
        co_return RespondNotFound(request);

    RT_LOG_INFO("blog.admin", "Comment {} is {} now{}", comment->Id, comment->Status,
        comment->Anchor ? Rooftree::StringFormat(", its passage {}", comment->Anchor->Status) : std::string());
    sWorld->RequestCommentCounts();
    sWorld->AnnounceChanged(comment->Target.ToString(), "comments");
    if (comment->Target.Kind == "thinking")
        sWorld->AnnounceChanged("thinking", "content");
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(CommentTable(builder, *comment));
    request->Respond(builder);
}
}
