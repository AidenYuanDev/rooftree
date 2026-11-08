/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Reactions: a logged in reader with a confirmed address gives a post or comment one
 * of a few kinds (like, heart, ...), each kind once, and may take it back. Pages ask for the counts of
 * many targets at once (a post and all its comments), with the reader's own reactions marked.
 */

#include "ApiCall.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Field.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Validation.h"
#include <map>
#include <set>

namespace http = boost::beast::http;

using Rooftree::Server::Session;

namespace Blog
{
namespace
{
constexpr std::size_t MaxTargetsPerRead = 100;
constexpr std::string_view ReactionKinds[] = { "like", "heart", "laugh", "party", "cat", "star", "eyes", "thinking", "thanks" };
constexpr std::string_view TargetKinds[] = { "post", "comment" };

struct ReactionTarget
{
    std::string Kind;
    uint32 Id = 0;

    std::string ToString() const { return Rooftree::StringFormat("{}:{}", Kind, Id); }
    auto operator<=>(ReactionTarget const&) const = default;
};

// "post:7", "comment:12"
Optional<ReactionTarget> ParseReactionTarget(std::string_view text)
{
    std::size_t colon = text.find(':');
    if (colon == std::string_view::npos)
        return {};

    std::string_view kind = text.substr(0, colon);
    Optional<uint32> id = Rooftree::StringTo<uint32>(text.substr(colon + 1));
    if (!id || !*id || std::ranges::find(TargetKinds, kind) == std::end(TargetKinds))
        return {};
    return ReactionTarget{ std::string(kind), *id };
}

// Worker: a published post, a visible comment
bool WorkerTargetExists(ReactionTarget const& target)
{
    BlogDatabaseStatements statement = target.Kind == "post" ? BLOG_SEL_POST_FOR_COMMENT : BLOG_SEL_COMMENT_FOR_REACTION;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(statement);
    stmt->setUInt32(0, target.Id);
    PreparedQueryResult row = BlogDatabase.Query(stmt);
    if (!row)
        return false;
    std::string status = row->Fetch()[0].GetString();
    return target.Kind == "comment" ? status == "visible" : status == "published";
}

// A target's reactions: the count of each kind, those of the login
struct TargetReactions
{
    std::string Target;     // post:7
    std::vector<std::pair<std::string, int64>> Counts;
    std::vector<std::string> Mine;

    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Reader::ReactionCount>>> CountsTable(flatbuffers::FlatBufferBuilder& builder) const
    {
        std::vector<flatbuffers::Offset<Reader::ReactionCount>> counts;
        for (auto const& [kind, count] : Counts)
            counts.push_back(Reader::CreateReactionCountDirect(builder, kind.c_str(), count));
        return builder.CreateVector(counts);
    }
};

// Worker: every target asked for, in order, also those without reactions. userId 0: nobody's own
std::vector<TargetReactions> WorkerLoadReactions(std::set<ReactionTarget> const& targets, uint32 userId)
{
    std::vector<TargetReactions> result;
    std::map<std::string, std::vector<uint32>> byKind;
    for (ReactionTarget const& target : targets)
    {
        result.push_back({ .Target = target.ToString() });
        byKind[target.Kind].push_back(target.Id);
    }
    if (targets.empty())
        return result;

    // Only kinds from TargetKinds and numbers go into the query
    std::string where;
    for (auto const& [kind, ids] : byKind)
    {
        std::string list;
        for (uint32 id : ids)
            list += (list.empty() ? "" : ",") + std::to_string(id);
        where += Rooftree::StringFormat("{}(target_kind = '{}' AND target_id IN ({}))", where.empty() ? "" : " OR ", kind, list);
    }

    std::string sql = Rooftree::StringFormat("SELECT target_kind, target_id, kind, COUNT(*), SUM(CASE WHEN user_id = {} THEN 1 ELSE 0 END) FROM reaction "
        "WHERE {} GROUP BY target_kind, target_id, kind ORDER BY kind", userId, where);
    if (QueryResult rows = BlogDatabase.Query(sql.c_str()))
    {
        do
        {
            Field* fields = rows->Fetch();
            std::string target = Rooftree::StringFormat("{}:{}", fields[0].GetString(), fields[1].GetUInt32());
            auto entry = std::ranges::find(result, target, &TargetReactions::Target);
            if (entry == result.end())
                continue;
            std::string kind = fields[2].GetString();
            entry->Counts.emplace_back(kind, fields[3].GetInt64());
            if (fields[4].GetInt64() > 0)
                entry->Mine.push_back(kind);
        } while (rows->NextRow());
    }
    return result;
}
}

Rooftree::Task<void> ListReactions(Session& session, ApiRequest request)
{
    // targets=post:7,comment:12,...
    std::set<ReactionTarget> targets;
    for (std::string const& text : SplitList(ReadText(request->Get<Reader::ReactionsListRequest>().targets())))
    {
        Optional<ReactionTarget> target = ParseReactionTarget(text);
        if (!target)
            co_return RespondValidation(request, { { "targets", "invalid" } });
        targets.insert(*target);
    }
    if (targets.size() > MaxTargetsPerRead)
        co_return RespondValidation(request, { { "targets", "too_long" } });

    uint32 userId = request->GetLogin() ? request->GetLogin()->User.Id : 0;
    std::vector<TargetReactions> reactions = co_await session.OnWorker([targets = std::move(targets), userId] { return WorkerLoadReactions(targets, userId); });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::TargetReactions>> items;
    for (TargetReactions const& target : reactions)
        items.push_back(Reader::CreateTargetReactions(builder, builder.CreateString(target.Target), target.CountsTable(builder),
            StringList(builder, target.Mine)));
    builder.Finish(Reader::CreateReactionsList(builder, builder.CreateVector(items)));
    request->Respond(builder);
}

Rooftree::Task<void> React(Session& session, ApiRequest request)
{
    AuthInfo const& auth = request->GetAuth();
    if (!auth.User.EmailVerified)
        co_return request->RespondError(http::status::forbidden, "email_unverified");
    if (auth.User.IsDenied(DenyReact))
        co_return request->RespondError(http::status::forbidden, "not_allowed");

    // { "target": "post:7", "kind": "like", "on": true }
    Reader::ReactRequest const& in = request->Get<Reader::ReactRequest>();
    TableFields fields(request->GetErrors());
    Optional<ReactionTarget> target = ParseReactionTarget(fields.Text("target", in.target(), 1, 30));
    if (!target)
        fields.AddError("target", "invalid");
    std::string kind = fields.Choice("kind", in.kind(), std::vector<std::string_view>(std::begin(ReactionKinds), std::end(ReactionKinds)));
    if (!in.on())
        fields.AddError("on", "required");
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    uint32 userId = auth.User.Id;
    struct Result
    {
        Optional<TargetReactions> State;
        std::string Page;           // what shows it: the target, a comment's post or guestbook
    };
    Result result = co_await session.OnWorker([target = *target, kind, on = *in.on(), userId]() -> Result
    {
        if (on)
        {
            if (!WorkerTargetExists(target))
                return {};

            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_REACTION);
            stmt->setUInt32(0, userId);
            stmt->setString(1, target.Kind);
            stmt->setUInt32(2, target.Id);
            stmt->setString(3, kind);
            for (uint8 column = 4; std::string_view targetKind : TargetKinds)
            {
                if (target.Kind == targetKind)
                    stmt->setUInt32(column, target.Id);
                else
                    stmt->setNull(column);
                ++column;
            }
            BlogDatabase.DirectExecute(stmt);
        }
        else
        {
            // Taking one back needs no check: there is nothing to delete for a target that's gone
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_REACTION);
            stmt->setUInt32(0, userId);
            stmt->setString(1, target.Kind);
            stmt->setUInt32(2, target.Id);
            stmt->setString(3, kind);
            BlogDatabase.DirectExecute(stmt);
        }

        Result result{ .State = std::move(WorkerLoadReactions({ target }, userId).front()), .Page = target.ToString() };
        if (target.Kind == "comment")
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_COMMENT);
            stmt->setUInt32(0, target.Id);
            PreparedQueryResult row = BlogDatabase.Query(stmt);
            Field* fields = row ? row->Fetch() : nullptr;
            result.Page = fields ? (fields[1].GetString() == "guestbook" ? std::string("guestbook")
                : Rooftree::StringFormat("{}:{}", fields[1].GetString(), fields[2].GetUInt32())) : std::string();
        }
        return result;
    });
    if (!result.State)
        co_return RespondNotFound(request);

    if (!result.Page.empty())
        sWorld->AnnounceChanged(std::move(result.Page), "reactions");
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateReactions(builder, result.State->CountsTable(builder), StringList(builder, result.State->Mine)));
    request->Respond(builder);
}
}
