/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Posts and tags: the public reads (lists by cursor, a post by slug, everything the prerender build
 * needs) and the admin's writes.
 *
 * A post has one translation per language and an original language that must always have one. Each
 * language's site has only the posts written in it: lists leave the others out, and a post asked for
 * in a language it lacks is not found (the answer names those it has). Changing
 * a post's slug keeps the old one as a redirect: the API answers it with 301 to the new one.
 *
 * Tags are few and read by every list, so BlogWorld keeps them in memory; they are reloaded after
 * every change that may change them (their post counts included).
 *
 * Also here: search of posts (in memory, SearchIndex.h), in the page's language unless asked
 * for all, and built from the same rows, the RSS feed of each language (posts and notes) and the
 * sitemap.
 */

#include "ApiCall.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Field.h"
#include "Log.h"
#include "Collab.h"
#include "PostAccess.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SearchIndex.h"
#include "SecurityUtils.h"
#include "StringFormat.h"
#include "StringConvert.h"
#include "Transaction.h"
#include "Validation.h"
#include <boost/json/array.hpp>
#include <fmt/chrono.h>
#include <array>
#include <map>
#include <set>
#include <sstream>

namespace http = boost::beast::http;

using Rooftree::Server::Session;

namespace Blog
{
namespace
{
constexpr uint32 DefaultPageSize = 10;
constexpr uint32 MaxPageSize = 50;
constexpr uint32 AdminPageSize = 50;
constexpr std::size_t MaxSlugLength = 100;
constexpr std::size_t MaxTagSlugLength = 50;
constexpr std::size_t MaxTagsPerPost = 20;

struct Translation
{
    std::string Lang;
    std::string Title;
    std::string Summary;
    std::string Body;
};

struct PostSummary
{
    uint32 Id = 0;
    std::string Slug;
    std::string Cover;
    SystemTimePoint PublishedAt;
    SystemTimePoint UpdatedAt;
    std::string OriginalLang;
    std::string Lang;
    std::string Title;
    std::string Summary;
    std::vector<std::string> Tags;
    std::vector<std::string> Langs;
    int64 CommentCount = 0;
};

struct PostRow
{
    uint32 Id = 0;
    std::string Slug;
    std::string Status;
    std::string OriginalLang;
    std::string Cover;
    bool CommentsEnabled = true;
    Optional<SystemTimePoint> PublishedAt;
    Optional<SystemTimePoint> PublishAt;    // a draft the server publishes then
    SystemTimePoint UpdatedAt;
    Optional<SystemTimePoint> CreatedAt;
    std::string Author;
    std::vector<std::string> Tags;
    std::vector<std::string> Langs;
    int64 CommentCount = 0;
    std::vector<Translation> Translations;
    // Its owner, by WorkerLoadPost
    uint32 OwnerId = 0;
    std::string OwnerUsername, OwnerDisplayName, OwnerAvatar;
    // The languages of its working copy, by WorkerLoadPost with its translations
    std::vector<std::string> DraftLangs;
    // Its publishing asked for, the admin's to do (the site's reviewWriters)
    Optional<SystemTimePoint> ReviewRequestedAt;

    bool IsPublished() const { return Status == "published"; }
};

flatbuffers::Offset<Admin::PostPerson> PostPersonTable(flatbuffers::FlatBufferBuilder& builder, uint32 id, std::string const& username,
    std::string const& displayName, std::string const& avatar)
{
    return Admin::CreatePostPerson(builder, id, builder.CreateString(username), builder.CreateString(displayName),
        avatar.empty() ? 0 : builder.CreateString("/uploads/" + avatar));
}

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Reader::TagName>>> TagNames(flatbuffers::FlatBufferBuilder& builder,
    std::vector<std::string> const& slugs, std::string_view lang)
{
    std::vector<flatbuffers::Offset<Reader::TagName>> tags;
    for (std::string const& slug : slugs)
    {
        TagInfo const* tag = sWorld->FindTag(slug);
        tags.push_back(Reader::CreateTagName(builder, builder.CreateString(slug), builder.CreateString(tag ? tag->GetName(lang) : slug)));
    }

    return builder.CreateVector(tags);
}

// A row of BLOG_SEL_POSTS
PostSummary ReadPostSummary(Field const* fields)
{
    PostSummary post;
    post.Id = fields[0].GetUInt32();
    post.Slug = fields[1].GetString();
    post.Cover = fields[2].GetString();
    post.PublishedAt = fields[3].GetDate();
    post.UpdatedAt = fields[4].GetDate();
    post.OriginalLang = fields[5].GetString();
    post.Lang = fields[6].GetString();
    post.Title = fields[7].GetString();
    post.Summary = fields[8].GetString();
    post.Tags = SplitList(fields[9].GetStringOrNull().value_or(""));
    post.Langs = SplitList(fields[10].GetStringOrNull().value_or(""));
    post.CommentCount = fields[11].GetInt64();
    return post;
}

flatbuffers::Offset<Reader::PostSummary> PostSummaryTable(flatbuffers::FlatBufferBuilder& builder, PostSummary const& post, std::string_view lang)
{
    return Reader::CreatePostSummary(builder, post.Id, builder.CreateString(post.Slug), builder.CreateString(post.Lang), post.Lang != lang,
        builder.CreateString(post.OriginalLang), builder.CreateString(post.Title), builder.CreateString(post.Summary),
        builder.CreateString(post.Cover), TagNames(builder, post.Tags, lang), StringList(builder, post.Langs), post.CommentCount,
        TimeString(builder, post.PublishedAt), TimeString(builder, post.UpdatedAt));
}

// The languages a tag has published posts in
std::vector<std::string> TagLangs(TagInfo const& tag)
{
    std::vector<std::string> langs;
    if (tag.PostCountEn)
        langs.emplace_back("en");
    if (tag.PostCountZh)
        langs.emplace_back("zh");
    return langs;
}

// The admin's, with the count of all published posts
void RespondTag(ApiRequest const& request, TagInfo const& tag, http::status status = http::status::ok)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Admin::CreateTagDirect(builder, tag.Slug.c_str(), tag.NameZh.c_str(), tag.NameEn.c_str(), tag.GetPostCount({})));
    request->Respond(builder, status);
}

// Worker: tags with their counts of published posts
std::vector<TagInfo> WorkerLoadTags()
{
    std::vector<TagInfo> tags;
    if (PreparedQueryResult result = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_TAGS)))
    {
        do
        {
            Field* fields = result->Fetch();
            tags.push_back({ .Id = fields[0].GetUInt32(), .Slug = fields[1].GetString(), .NameZh = fields[2].GetString(),
                .NameEn = fields[3].GetString(), .PostCount = uint32(fields[4].GetInt64()), .PostCountZh = uint32(fields[5].GetInt64()),
                .PostCountEn = uint32(fields[6].GetInt64()) });
        } while (result->NextRow());
    }

    return tags;
}

Optional<PostRow> WorkerLoadPost(uint32 id, bool withTranslations)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_BY_ID);
    stmt->setUInt32(0, id);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    Field* fields = result->Fetch();
    PostRow post;
    post.Id = fields[0].GetUInt32();
    post.Slug = fields[1].GetString();
    post.Status = fields[2].GetString();
    post.OriginalLang = fields[3].GetString();
    post.Cover = fields[4].GetString();
    post.CommentsEnabled = fields[5].GetUInt8() != 0;
    post.PublishedAt = fields[6].GetDateOrNull();
    post.UpdatedAt = fields[7].GetDate();
    post.CreatedAt = fields[8].GetDate();
    post.Tags = SplitList(fields[9].GetStringOrNull().value_or(""));
    post.PublishAt = fields[10].GetDateOrNull();
    post.OwnerId = fields[11].GetUInt32();
    post.OwnerUsername = fields[12].GetString();
    post.OwnerDisplayName = fields[13].GetString();
    post.OwnerAvatar = fields[14].GetStringOrNull().value_or("");
    post.ReviewRequestedAt = fields[15].GetDateOrNull();

    if (withTranslations)
    {
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_TRANSLATIONS);
        stmt->setUInt32(0, id);
        if (PreparedQueryResult translations = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* row = translations->Fetch();
                post.Translations.push_back({ row[0].GetString(), row[1].GetString(), row[2].GetString(), row[3].GetString() });
                post.Langs.push_back(row[0].GetString());
            } while (translations->NextRow());
        }
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_DRAFT_LANGS);
        stmt->setUInt32(0, id);
        if (PreparedQueryResult langs = BlogDatabase.Query(stmt))
        {
            do
                post.DraftLangs.push_back(langs->Fetch()[0].GetString());
            while (langs->NextRow());
        }
    }

    return post;
}

// Worker: the post's word count, of its original language, for the home page's total
void WorkerUpdatePostWords(PostRow const& post)
{
    for (Translation const& translation : post.Translations)
    {
        if (translation.Lang != post.OriginalLang)
            continue;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_POST_WORDS);
        stmt->setUInt32(0, uint32(CountWords(translation.Title) + CountWords(translation.Body)));
        stmt->setUInt32(1, post.Id);
        BlogDatabase.DirectExecute(stmt);
    }
}

// Worker, after a post changed: its published version in each language is its newest published
// revision (all of them are kept), and the passages comments are on are looked for in what changed. One whose text
// is gone is orphaned (the admin decides, its author may choose another), one whose text is back is
// ok again; one the admin kept stays so while its text is gone. A draft changes nothing. True when a
// comment's passage changed status
bool WorkerSyncPostRevisions(uint32 postId, Optional<uint32> by)
{
    struct Text
    {
        std::string Title;
        std::string Body;
        std::string Summary;
    };

    std::map<std::string, Text> texts;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_TEXTS);
    stmt->setUInt32(0, postId);
    PreparedQueryResult rows = BlogDatabase.Query(stmt);
    if (!rows || rows->Fetch()[0].GetString() != "published")
        return false;
    do
    {
        Field* fields = rows->Fetch();
        texts[fields[1].GetString()] = { fields[2].GetString(), fields[3].GetString(), fields[4].GetString() };
    } while (rows->NextRow());

    std::set<std::string> changed;
    for (auto const& [lang, text] : texts)
    {
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_LATEST_REVISION_TEXT);
        stmt->setUInt32(0, postId);
        stmt->setString(1, lang);
        stmt->setString(2, std::string("published"));
        stmt->setString(3, std::string("published"));
        PreparedQueryResult latest = BlogDatabase.Query(stmt);
        bool same = latest && latest->Fetch()[0].GetString() == text.Title && latest->Fetch()[1].GetString() == text.Body;
        // Passages are on its title and Markdown; a summary changed alone is a version too
        if (same && latest->Fetch()[2].GetString() == text.Summary)
            continue;

        if (!same)
            changed.insert(lang);
        stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_POST_REVISION);
        stmt->setUInt32(0, postId);
        stmt->setString(1, lang);
        stmt->setString(2, std::string("published"));
        stmt->setString(3, text.Title);
        stmt->setString(4, text.Summary);
        stmt->setString(5, text.Body);
        if (by)
            stmt->setUInt32(6, *by);
        else
            stmt->setNull(6);
        stmt->setString(7, std::string());
        BlogDatabase.DirectExecute(stmt);
    }

    // A language that went has no text: its passages are all gone
    std::map<std::string, std::u32string> matchable;
    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_PASSAGES);
    stmt->setUInt32(0, postId);
    PreparedQueryResult passages = BlogDatabase.Query(stmt);
    if (!passages)
        return false;
    bool passagesChanged = false;
    do
    {
        Field* fields = passages->Fetch();
        std::string lang = fields[1].GetString();
        auto text = texts.find(lang);
        if (text != texts.end() && !changed.contains(lang))
            continue;

        auto [itr, added] = matchable.try_emplace(lang);
        if (added && text != texts.end())
            itr->second = MatchableText(text->second.Body);
        std::u32string quote = MatchableText(fields[2].GetString());
        bool found = !quote.empty() && itr->second.find(quote) != std::u32string::npos;
        std::string status = fields[3].GetString();
        std::string wanted = found ? "ok" : status == "kept" ? "kept" : "orphaned";
        if (wanted == status)
            continue;

        stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_COMMENT_ANCHOR_STATUS);
        stmt->setString(0, wanted);
        stmt->setUInt32(1, fields[0].GetUInt32());
        BlogDatabase.DirectExecute(stmt);
        passagesChanged = true;
    } while (passages->NextRow());
    return passagesChanged;
}

struct Neighbour
{
    std::string Slug;
    std::string Title;
};

// The neighbours of a published post
Optional<Neighbour> WorkerFindNeighbour(BlogDatabaseStatements statement, std::string const& lang, SystemTimePoint publishedAt, uint32 id)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(statement);
    stmt->setString(0, lang);
    stmt->setDate(1, publishedAt);
    stmt->setDate(2, publishedAt);
    stmt->setUInt32(3, id);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    Field* fields = result->Fetch();
    return Neighbour{ fields[0].GetString(), fields[1].GetString() };
}

flatbuffers::Offset<Reader::Neighbour> NeighbourTable(flatbuffers::FlatBufferBuilder& builder, Optional<Neighbour> const& neighbour)
{
    return neighbour ? Reader::CreateNeighbourDirect(builder, neighbour->Slug.c_str(), neighbour->Title.c_str()) : 0;
}

std::string OriginalTitle(PostRow const& post)
{
    for (Translation const& translation : post.Translations)
        if (translation.Lang == post.OriginalLang)
            return translation.Title;
    return post.Slug;
}

Optional<uint32> WorkerFindPostId(std::string const& slug)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_ID_BY_SLUG);
    stmt->setString(0, slug);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    return result->Fetch()[0].GetUInt32();
}

flatbuffers::Offset<Admin::Post> AdminPostTable(flatbuffers::FlatBufferBuilder& builder, PostRow const& post, PostAccess const& access)
{
    flatbuffers::Offset<Admin::PostText> texts[2];
    for (Translation const& translation : post.Translations)
        texts[translation.Lang == "en"] = Admin::CreatePostTextDirect(builder, translation.Title.c_str(), translation.Summary.c_str(),
            translation.Body.c_str());
    auto translations = Admin::CreatePostTranslations(builder, texts[0], texts[1]);
    return Admin::CreatePost(builder, post.Id, builder.CreateString(post.Slug), builder.CreateString(post.Status),
        builder.CreateString(post.OriginalLang), builder.CreateString(post.Cover), post.CommentsEnabled, StringList(builder, post.Tags),
        translations, TimeString(builder, post.PublishedAt), TimeString(builder, post.PublishAt), TimeString(builder, post.UpdatedAt),
        TimeString(builder, post.CreatedAt), PostPersonTable(builder, post.OwnerId, post.OwnerUsername, post.OwnerDisplayName, post.OwnerAvatar),
        StringList(builder, access.List()), StringList(builder, post.DraftLangs), TimeString(builder, post.ReviewRequestedAt));
}

void RespondPost(ApiRequest const& request, PostRow const& post, PostAccess const& access, http::status status = http::status::ok)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(AdminPostTable(builder, post, access));
    request->Respond(builder, status);
}

// The fields of a create (all of them) or an update (any of them)
struct PostInput
{
    Optional<std::string> Slug;
    Optional<std::string> Status;
    Optional<std::string> OriginalLang;
    Optional<std::string> Cover;
    Optional<bool> CommentsEnabled;
    Optional<std::vector<std::string>> Tags;
    std::map<std::string, Optional<Translation>> Translations;  // nullopt: delete that language
    Optional<Optional<SystemTimePoint>> PublishAt;              // absent: unchanged; none: not scheduled
    Optional<std::string> Owner;                                // the user name of its new owner
    Optional<bool> ReviewRequested;                             // false: not asked to be published any more
};

// "2026-10-30T08:00:00Z", with or without seconds' fractions (what Date.toISOString() writes)
Optional<SystemTimePoint> ParseIsoTime(std::string_view text)
{
    if (text.size() < 20 || !text.ends_with('Z') || (text.size() > 20 && text[19] != '.'))
        return {};
    std::istringstream in{ std::string(text.substr(0, 19)) };
    std::chrono::sys_seconds time;
    in >> std::chrono::parse("%Y-%m-%dT%H:%M:%S", time);
    if (in.fail())
        return {};
    return SystemTimePoint(time);
}

bool IsValidCover(std::string_view cover)
{
    return cover.empty() || cover.starts_with("/uploads/") || cover.starts_with("https://");
}

// Empty with the errors in fields if the body is invalid
Optional<PostInput> ReadPostInput(bool creating, Admin::PostInput const& in, TableFields& fields)
{
    PostInput input;
    if (creating || in.slug())
        input.Slug = fields.Slug("slug", in.slug(), MaxSlugLength);
    if (creating || in.status())
    {
        input.Status = fields.Choice("status", in.status(), { "draft", "published" }, false);
        if (input.Status->empty())
            input.Status = "draft";
    }
    if (creating || in.originalLang())
        input.OriginalLang = fields.Lang("originalLang", in.originalLang());
    if (creating || in.cover())
    {
        input.Cover = fields.Text("cover", in.cover(), 0, 255, false);
        if (!IsValidCover(*input.Cover))
            fields.AddError("cover", "invalid");
    }
    if (in.commentsEnabled())
        input.CommentsEnabled = *in.commentsEnabled();
    // When the server publishes the draft: in the future, at most a year ahead; "" (null) not at all
    if (flatbuffers::String const* value = in.publishAt())
    {
        input.PublishAt.emplace();
        if (value->size())
        {
            SystemTimePoint now = SystemTimePoint::clock::now();
            Optional<SystemTimePoint> at = ParseIsoTime(value->string_view());
            if (!at || *at > now + std::chrono::days(366))
                fields.AddError("publishAt", "invalid");
            else if (*at <= now)
                fields.AddError("publishAt", "past");
            else
                *input.PublishAt = at;
        }
    }
    if (in.tags())
    {
        input.Tags = fields.SlugList("tags", in.tags(), MaxTagsPerPost, MaxTagSlugLength);
        for (std::string const& tag : *input.Tags)
            if (!sWorld->FindTag(tag))
                fields.AddError("tags", "unknown_tag");
    }

    if (Admin::PostTranslations const* translations = in.translations())
    {
        for (auto [lang, text] : { std::pair("zh", translations->zh()), std::pair("en", translations->en()) })
        {
            if (!text)
                continue;

            std::string langName(lang);
            // An empty one (null) deletes the language
            if (IsEmptyTable(text) && !creating)
            {
                input.Translations[langName] = std::nullopt;
                continue;
            }

            fields.SetPrefix("translations." + langName);
            Translation translation;
            translation.Lang = langName;
            translation.Title = fields.Text("title", text->title(), 1, 200);
            translation.Summary = fields.Text("summary", text->summary(), 0, 500, false);
            translation.Body = fields.Text("body", text->body(), 1, MaxBodyLength, true, true);
            fields.SetPrefix({});
            input.Translations[langName] = std::move(translation);
        }
    }
    else if (creating)
        fields.AddError("translations", "required");

    if (creating && input.OriginalLang && !input.OriginalLang->empty() && !input.Translations.contains(*input.OriginalLang))
        fields.AddError("translations." + *input.OriginalLang, "required");
    if (!creating && in.owner())
        input.Owner = fields.Text("owner", in.owner(), 1, 100);
    if (!creating && in.reviewRequested())
    {
        if (*in.reviewRequested())
            fields.AddError("reviewRequested", "invalid");
        input.ReviewRequested = false;
    }

    if (!fields.IsValid())
        return {};

    return input;
}
}

std::string GetLang(BlogSession::Request const& request)
{
    Optional<std::string> lang = request->GetQueryParameter("lang");
    return lang && IsValidLang(*lang) ? *lang : "zh";
}

// "<seconds>.<id>" of the last post of a page, base64url
std::string EncodeCursor(SystemTimePoint publishedAt, uint32 id)
{
    std::string text = Rooftree::StringFormat("{}.{}", std::chrono::duration_cast<Seconds>(publishedAt.time_since_epoch()).count(), id);
    return Rooftree::Server::Base64UrlEncode(std::span(reinterpret_cast<uint8 const*>(text.data()), text.size()));
}

Optional<std::pair<SystemTimePoint, uint32>> DecodeCursor(std::string_view cursor)
{
    Optional<std::vector<uint8>> bytes = Rooftree::Server::Base64UrlDecode(cursor);
    if (!bytes || bytes->size() > 40)
        return {};

    std::string_view text(reinterpret_cast<char const*>(bytes->data()), bytes->size());
    std::size_t dot = text.find('.');
    if (dot == std::string_view::npos)
        return {};

    Optional<int64> seconds = Rooftree::StringTo<int64>(text.substr(0, dot));
    Optional<uint32> id = Rooftree::StringTo<uint32>(text.substr(dot + 1));
    if (!seconds || !id)
        return {};

    return std::make_pair(SystemTimePoint(Seconds(*seconds)), *id);
}

Rooftree::Task<void> ListPosts(Session& session, ApiRequest request)
{
    Reader::PostsListRequest const& in = request->Get<Reader::PostsListRequest>();
    std::string lang = ReadLang(in.lang());
    std::string tag = ReadText(in.tag());
    uint32 limit = std::clamp(in.limit().value_or(DefaultPageSize), 1u, MaxPageSize);

    // The first page starts after everything
    SystemTimePoint beforeTime = PermanentBanTime();
    uint32 beforeId = std::numeric_limits<uint32>::max();
    if (in.cursor() && in.cursor()->size())
    {
        Optional<std::pair<SystemTimePoint, uint32>> decoded = DecodeCursor(in.cursor()->string_view());
        if (!decoded)
            co_return request->RespondError(http::status::bad_request, "invalid_cursor");
        std::tie(beforeTime, beforeId) = *decoded;
    }

    std::string key = Rooftree::StringFormat("posts|{}|{}|{}|{}", lang, tag, limit, ReadText(in.cursor()));
    ReadCache& cache = sWorld->GetReadCache();
    if (std::string const* answer = cache.Find(key))
        co_return request->Respond(*answer);

    auto generation = cache.GetGeneration();
    std::vector<PostSummary> posts = co_await session.OnWorker([=]
    {
        std::vector<PostSummary> posts;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POSTS);
        stmt->setString(0, lang);
        stmt->setString(1, tag);
        stmt->setString(2, tag);
        stmt->setDate(3, beforeTime);
        stmt->setDate(4, beforeTime);
        stmt->setUInt32(5, beforeId);
        // One more than a page tells whether there is a next one
        stmt->setUInt32(6, limit + 1);
        if (PreparedQueryResult result = BlogDatabase.Query(stmt))
        {
            do
                posts.push_back(ReadPostSummary(result->Fetch()));
            while (result->NextRow());
        }

        return posts;
    });
    std::string nextCursor;
    if (posts.size() > limit)
    {
        posts.resize(limit);
        nextCursor = EncodeCursor(posts.back().PublishedAt, posts.back().Id);
    }

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::PostSummary>> items;
    for (PostSummary const& post : posts)
        items.push_back(PostSummaryTable(builder, post, lang));
    builder.Finish(Reader::CreatePostsList(builder, builder.CreateVector(items), nextCursor.empty() ? 0 : builder.CreateString(nextCursor)));
    std::string answer = Bytes(builder);
    sWorld->GetReadCache().Store(std::move(key), answer, generation);
    request->Respond(std::move(answer));
}

Rooftree::Task<void> GetPost(Session& session, ApiRequest request)
{
    Reader::PostsGetRequest const& in = request->Get<Reader::PostsGetRequest>();
    std::string slug = ReadText(in.slug());
    std::string lang = ReadLang(in.lang());
    if (!IsValidSlug(slug, MaxSlugLength))
        co_return RespondNotFound(request);

    std::string key = Rooftree::StringFormat("post|{}|{}", slug, lang);
    ReadCache& cache = sWorld->GetReadCache();
    if (std::string const* answer = cache.Find(key))
        co_return request->Respond(*answer);

    struct Result
    {
        Optional<PostRow> Post;
        Optional<Translation> Text;
        std::vector<std::string> Langs;     // of a post not in lang
        std::string RedirectTo;
        Optional<Neighbour> Previous;
        Optional<Neighbour> Next;
        std::vector<std::array<std::string, 4>> People;     // id, user name, display name, avatar
    };

    auto generation = cache.GetGeneration();
    Result result = co_await session.OnWorker([slug, lang]
    {
        Result result;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_BY_SLUG);
        stmt->setString(0, slug);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_REDIRECT);
            stmt->setString(0, slug);
            if (PreparedQueryResult redirect = BlogDatabase.Query(stmt))
                result.RedirectTo = redirect->Fetch()[0].GetString();
            return result;
        }

        Field* fields = row->Fetch();
        PostRow post;
        post.Id = fields[0].GetUInt32();
        post.Slug = fields[1].GetString();
        post.Status = fields[2].GetString();
        post.OriginalLang = fields[3].GetString();
        post.Cover = fields[4].GetString();
        post.CommentsEnabled = fields[5].GetUInt8() != 0;
        post.PublishedAt = fields[6].GetDateOrNull();
        post.UpdatedAt = fields[7].GetDate();
        post.Author = fields[8].GetString();
        post.Tags = SplitList(fields[9].GetStringOrNull().value_or(""));
        post.Langs = SplitList(fields[10].GetStringOrNull().value_or(""));
        post.CommentCount = fields[11].GetInt64();
        if (!post.IsPublished())
            return result;

        if (std::ranges::find(post.Langs, lang) == post.Langs.end())
        {
            result.Langs = std::move(post.Langs);
            return result;
        }
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_TRANSLATION);
        stmt->setUInt32(0, post.Id);
        stmt->setString(1, lang);
        if (PreparedQueryResult text = BlogDatabase.Query(stmt))
        {
            Field* t = text->Fetch();
            result.Text = Translation{ t[0].GetString(), t[1].GetString(), t[2].GetString(), t[3].GetString() };
        }

        if (post.PublishedAt)
        {
            result.Previous = WorkerFindNeighbour(BLOG_SEL_POST_PREV, lang, *post.PublishedAt, post.Id);
            result.Next = WorkerFindNeighbour(BLOG_SEL_POST_NEXT, lang, *post.PublishedAt, post.Id);
        }
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_PEOPLE);
        stmt->setUInt32(0, post.Id);
        stmt->setUInt32(1, post.Id);
        if (PreparedQueryResult people = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* person = people->Fetch();
                result.People.push_back({ std::to_string(person[0].GetUInt32()), person[1].GetString(), person[2].GetString(),
                    person[3].GetStringOrNull().value_or("") });
            } while (people->NextRow());
        }
        result.Post = std::move(post);
        return result;
    });
    // fetch() follows the route's Location; the details name the canonical slug for clients that don't
    if (!result.RedirectTo.empty())
        co_return request->RespondError({ .Status = uint16(http::status::moved_permanently), .Code = "moved", .Details = {
            { .Name = "redirect", .Value = result.RedirectTo },
            { .Name = "location", .Value = Rooftree::StringFormat("/api/posts/{}?lang={}", result.RedirectTo, lang) } } });

    if (!result.Langs.empty())
        co_return RespondNotInLang(request, result.Langs);
    if (!result.Post || !result.Text)
        co_return RespondNotFound(request);

    PostRow const& post = *result.Post;
    Translation const& text = *result.Text;
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::Contributor>> people;
    for (auto const& [id, username, displayName, avatar] : result.People)
        people.push_back(Reader::CreateContributor(builder, Rooftree::StringTo<uint32>(id).value_or(0), builder.CreateString(username),
            builder.CreateString(displayName), avatar.empty() ? 0 : builder.CreateString("/uploads/" + avatar)));
    builder.Finish(Reader::CreatePost(builder, post.Id, builder.CreateString(post.Slug), builder.CreateString(text.Lang), false,
        builder.CreateString(post.OriginalLang), StringList(builder, post.Langs), builder.CreateString(text.Title),
        builder.CreateString(text.Summary), builder.CreateString(text.Body), builder.CreateString(post.Cover), builder.CreateString(post.Author),
        TagNames(builder, post.Tags, lang), post.CommentsEnabled, post.CommentCount, TimeString(builder, post.PublishedAt),
        TimeString(builder, post.UpdatedAt), NeighbourTable(builder, result.Previous), NeighbourTable(builder, result.Next),
        builder.CreateVector(people)));
    std::string answer = Bytes(builder);
    sWorld->GetReadCache().Store(std::move(key), answer, generation);
    request->Respond(std::move(answer));
}

// Someone's page: who they are and what they wrote, owned or edited
Rooftree::Task<void> GetPerson(Session& session, ApiRequest request)
{
    Reader::PersonGetRequest const& in = request->Get<Reader::PersonGetRequest>();
    std::string username = in.username() ? in.username()->str() : std::string();
    std::string lang = in.lang() && in.lang()->string_view() == "en" ? "en" : "zh";
    if (username.empty() || username.size() > 100)
        co_return RespondNotFound(request);

    std::string key = Rooftree::StringFormat("person|{}|{}", username, lang);
    ReadCache& cache = sWorld->GetReadCache();
    if (std::string const* answer = cache.Find(key))
        co_return request->Respond(*answer);

    auto generation = cache.GetGeneration();
    Optional<std::string> answer = co_await session.OnWorker([username, lang]() -> Optional<std::string>
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_PERSON);
        stmt->setString(0, username);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
            return {};
        Field* fields = row->Fetch();
        uint32 id = fields[0].GetUInt32();

        flatbuffers::FlatBufferBuilder builder;
        std::string avatar = fields[3].GetStringOrNull().value_or("");
        auto person = Reader::CreateContributor(builder, id, builder.CreateString(fields[1].GetString()), builder.CreateString(fields[2].GetString()),
            avatar.empty() ? 0 : builder.CreateString("/uploads/" + avatar));
        auto since = TimeString(builder, fields[4].GetDate());

        std::vector<flatbuffers::Offset<Reader::PostSummary>> posts;
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_PERSON_POSTS);
        stmt->setString(0, lang);
        stmt->setUInt32(1, id);
        stmt->setUInt32(2, id);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
                posts.push_back(PostSummaryTable(builder, ReadPostSummary(rows->Fetch()), lang));
            while (rows->NextRow());
        }
        builder.Finish(Reader::CreatePerson(builder, person, since, builder.CreateVector(posts)));
        return Bytes(builder);
    });
    if (!answer)
        co_return RespondNotFound(request);
    sWorld->GetReadCache().Store(std::move(key), *answer, generation);
    request->Respond(std::move(*answer));
}

Rooftree::Task<void> ListSlugs(Session& session, ApiRequest request)
{
    struct Slug
    {
        std::string Slug;
        SystemTimePoint UpdatedAt;
        std::vector<std::string> Langs;
    };

    struct Result
    {
        std::vector<Slug> Posts;
        std::vector<std::pair<std::string, std::string>> Redirects;
    };

    Result result = co_await session.OnWorker([]
    {
        Result result;
        if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_PUBLISHED_SLUGS)))
        {
            do
            {
                Field* fields = rows->Fetch();
                result.Posts.push_back({ fields[0].GetString(), fields[1].GetDate(), SplitList(fields[2].GetStringOrNull().value_or("")) });
            } while (rows->NextRow());
        }

        if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_PUBLISHED_REDIRECTS)))
        {
            do
            {
                Field* fields = rows->Fetch();
                result.Redirects.emplace_back(fields[0].GetString(), fields[1].GetString());
            } while (rows->NextRow());
        }

        return result;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::SlugItem>> posts;
    for (Slug const& post : result.Posts)
        posts.push_back(Reader::CreateSlugItem(builder, builder.CreateString(post.Slug), TimeString(builder, post.UpdatedAt),
            StringList(builder, post.Langs)));
    std::vector<flatbuffers::Offset<Reader::SlugRedirect>> redirects;
    for (auto const& [from, to] : result.Redirects)
        redirects.push_back(Reader::CreateSlugRedirectDirect(builder, from.c_str(), to.c_str()));
    // Each in the languages it has posts in
    std::vector<flatbuffers::Offset<Reader::TagLangs>> tags;
    for (auto const& [slug, tag] : sWorld->GetTags())
        if (tag.PostCount)
            tags.push_back(Reader::CreateTagLangs(builder, builder.CreateString(slug), StringList(builder, TagLangs(tag))));

    builder.Finish(Reader::CreatePostsSlugs(builder, builder.CreateVector(posts), builder.CreateVector(redirects), builder.CreateVector(tags)));
    request->Respond(builder);
}

Rooftree::Task<void> ListTags(Session& /*session*/, ApiRequest request)
{
    // The counts in lang; without one (the admin), of all posts
    std::string lang = ReadText(request->Get<Reader::TagsListRequest>().lang());
    if (!lang.empty() && !IsValidLang(lang))
        lang = "zh";

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::Tag>> tags;
    for (auto const& [slug, tag] : sWorld->GetTags())
        tags.push_back(Reader::CreateTagDirect(builder, tag.Slug.c_str(), tag.NameZh.c_str(), tag.NameEn.c_str(), tag.GetPostCount(lang)));
    builder.Finish(Reader::CreateTagsList(builder, builder.CreateVector(tags)));
    request->Respond(builder);
    co_return;
}

// Main thread: publishing by user asks the admin for it instead (the site's reviewWriters)
bool NeedsReview(UserRow const& user)
{
    if (user.IsAdmin())
        return false;
    boost::json::object const& profile = sWorld->GetSiteProfile();
    boost::json::value const* review = profile.if_contains("reviewWriters");
    return review && review->is_bool() && review->as_bool();
}

// Worker: publishing asked for by userId, or (none) no longer
void WorkerSetReview(uint32 postId, Optional<uint32> userId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_POST_REVIEW);
    if (userId)
    {
        stmt->setDate(0, SystemTimePoint::clock::now());
        stmt->setUInt32(1, *userId);
    }
    else
    {
        stmt->setNull(0);
        stmt->setNull(1);
    }
    stmt->setUInt32(2, postId);
    BlogDatabase.DirectExecute(stmt);
}

// Worker: the editors' newest text (Collab.h) written to the working copy, each document's with its
// version: what was written meanwhile, newer, stays
void WorkerWriteSnapshots(uint32 postId, std::vector<DocSnapshot> const& snapshots)
{
    for (DocSnapshot const& snapshot : snapshots)
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(snapshot.Key.Field == "title" ? BLOG_UPS_POST_DRAFT_TITLE
            : snapshot.Key.Field == "summary" ? BLOG_UPS_POST_DRAFT_SUMMARY : BLOG_UPS_POST_DRAFT_BODY);
        stmt->setUInt32(0, postId);
        stmt->setString(1, snapshot.Key.Lang);
        stmt->setString(2, snapshot.Text);
        stmt->setUInt32(3, snapshot.Version);
        BlogDatabase.DirectExecute(stmt);
    }
}

// Worker: the editors' newest text (Collab.h) written to the working copy, then each language's
// working copy copied to what readers see, in trans; the languages in except stay as they are. The
// field that can't be published ("translations.en.title") if one can't, and nothing is
Optional<std::string> WorkerApplyDrafts(uint32 postId, std::vector<DocSnapshot> const& snapshots, BlogDatabaseTransaction& trans,
    std::set<std::string> const& except = {})
{
    WorkerWriteSnapshots(postId, snapshots);

    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_DRAFTS);
    stmt->setUInt32(0, postId);
    PreparedQueryResult rows = BlogDatabase.Query(stmt);
    if (!rows)
        return {};
    std::vector<Translation> drafts;
    do
    {
        Field* fields = rows->Fetch();
        std::string lang = fields[0].GetString();
        if (except.contains(lang))
            continue;
        Translation draft{ lang, fields[1].GetStringOrNull().value_or(""), fields[2].GetString(), fields[3].GetStringOrNull().value_or("") };
        // A language begun but not written yet isn't one readers get
        if (draft.Title.empty() && draft.Body.empty() && fields[1].IsNull())
            continue;
        if (draft.Title.find_first_not_of(" \t\n\r") == std::string::npos)
            return "translations." + lang + ".title";
        if (draft.Body.find_first_not_of(" \t\n\r") == std::string::npos)
            return "translations." + lang + ".body";
        drafts.push_back(std::move(draft));
    } while (rows->NextRow());

    for (Translation const& draft : drafts)
    {
        stmt = BlogDatabase.GetPreparedStatement(BLOG_REP_POST_TRANSLATION);
        stmt->setUInt32(0, postId);
        stmt->setString(1, draft.Lang);
        stmt->setString(2, draft.Title);
        stmt->setString(3, draft.Summary);
        stmt->setString(4, draft.Body);
        trans->Append(stmt);
    }
    return {};
}

Optional<PostAccess> WorkerPostAccess(uint32 postId, UserRow const& user)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_ACCESS);
    stmt->setUInt32(0, user.Id);
    stmt->setUInt32(1, postId);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    Field* fields = result->Fetch();
    PostAccess access;
    access.Admin = user.IsAdmin();
    access.OwnerId = fields[0].GetUInt32();
    access.Owner = access.OwnerId == user.Id;
    access.Editor = fields[1].GetInt64() != 0;
    access.Rights = SplitList(fields[2].GetStringOrNull().value_or(""));
    return access;
}

// A row of BLOG_SEL_ADMIN_POSTS
struct AdminPostItem
{
    uint32 Id;
    std::string Slug, Status, OriginalLang;
    SystemTimePoint UpdatedAt;
    Optional<SystemTimePoint> PublishedAt;
    std::string Title;
    std::vector<std::string> Langs;
    Optional<SystemTimePoint> PublishAt;
    bool Mine = false;
    bool ReviewRequested = false;
};

// The admin's: every post. A writer's or an editor's: the ones it owns or edits
Rooftree::Task<void> AdminListPosts(Session& session, ApiRequest request)
{
    Admin::PostsListRequest const& in = request->Get<Admin::PostsListRequest>();
    std::string status = in.status() ? in.status()->str() : std::string();
    if (!status.empty() && status != "draft" && status != "published" && status != "review")
        co_return RespondValidation(request, { { "status", "invalid" } });

    uint32 page = std::max(1u, in.page().value_or(1));
    UserRow const& user = request->GetAuth().User;
    std::vector<AdminPostItem> posts = co_await session.OnWorker([status, page, userId = user.Id, admin = user.IsAdmin()]
    {
        std::vector<AdminPostItem> posts;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(admin ? BLOG_SEL_ADMIN_POSTS : BLOG_SEL_EDITOR_POSTS);
        uint8 at = 0;
        if (!admin)
        {
            stmt->setUInt32(at++, userId);
            stmt->setUInt32(at++, userId);
        }
        stmt->setString(at++, status);
        stmt->setString(at++, status);
        stmt->setString(at++, status);
        stmt->setUInt32(at++, AdminPageSize + 1);
        stmt->setUInt32(at++, (page - 1) * AdminPageSize);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                posts.push_back({ .Id = fields[0].GetUInt32(), .Slug = fields[1].GetString(), .Status = fields[2].GetString(),
                    .OriginalLang = fields[3].GetString(), .UpdatedAt = fields[4].GetDate(), .PublishedAt = fields[5].GetDateOrNull(),
                    .Title = fields[6].GetString(), .Langs = SplitList(fields[7].GetStringOrNull().value_or("")),
                    .PublishAt = fields[8].GetDateOrNull(), .Mine = fields[9].GetUInt32() == userId,
                    .ReviewRequested = fields[10].GetInt64() != 0 });
            } while (rows->NextRow());
        }

        return posts;
    });
    bool hasMore = posts.size() > AdminPageSize;
    if (hasMore)
        posts.resize(AdminPageSize);

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::PostsListItem>> items;
    for (AdminPostItem const& post : posts)
        items.push_back(Admin::CreatePostsListItem(builder, post.Id, builder.CreateString(post.Slug), builder.CreateString(post.Status),
            builder.CreateString(post.OriginalLang), TimeString(builder, post.UpdatedAt), TimeString(builder, post.PublishedAt),
            builder.CreateString(post.Title), StringList(builder, post.Langs), TimeString(builder, post.PublishAt), post.Mine, post.ReviewRequested));
    builder.Finish(Admin::CreatePostsList(builder, builder.CreateVector(items), hasMore));
    request->Respond(builder);
}

Rooftree::Task<void> AdminGetPost(Session& session, ApiRequest request)
{
    flatbuffers::Optional<uint32> id = request->Get<Admin::IdRequest>().id();
    if (!id)
        co_return RespondNotFound(request);

    struct Result
    {
        Optional<PostAccess> Access;
        Optional<PostRow> Post;
    };

    // One it doesn't edit is one it doesn't know of
    Result result = co_await session.OnWorker([id = *id, user = request->GetAuth().User]
    {
        Result result;
        result.Access = WorkerPostAccess(id, user);
        if (result.Access && result.Access->CanEdit())
            result.Post = WorkerLoadPost(id, true);
        return result;
    });
    if (!result.Post)
        co_return RespondNotFound(request);

    RespondPost(request, *result.Post, *result.Access);
}

Rooftree::Task<void> AdminCreatePost(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    if (!auth->User.IsGranted(GrantWrite))
        co_return request->RespondError(http::status::forbidden, "not_allowed");

    TableFields fields(request->GetErrors());
    Optional<PostInput> input = ReadPostInput(true, request->Get<Admin::PostInput>(), fields);
    if (!input)
        co_return RespondValidation(request, fields.TakeErrors());

    struct Result
    {
        bool SlugTaken = false;
        Optional<PostRow> Post;
        std::vector<TagInfo> Tags;
    };

    // With reviews, a writer's post published at once is one asked to be; it can't wait for a time
    bool review = NeedsReview(auth->User);
    if (review && input->PublishAt && *input->PublishAt)
        co_return RespondValidation(request, { { "publishAt", "review_required" } });
    bool askReview = review && *input->Status == "published";
    if (askReview)
        input->Status = "draft";

    uint32 authorId = auth->User.Id;
    Result result = co_await session.OnWorker([input = std::move(*input), authorId, askReview]
    {
        Result result;
        if (WorkerFindPostId(*input.Slug))
        {
            result.SlugTaken = true;
            return result;
        }

        bool published = *input.Status == "published";
        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_POST);
        stmt->setString(0, *input.Slug);
        stmt->setString(1, *input.Status);
        stmt->setString(2, *input.OriginalLang);
        stmt->setString(3, *input.Cover);
        stmt->setUInt32(4, authorId);
        stmt->setBool(5, input.CommentsEnabled.value_or(true));
        if (published)
            stmt->setDate(6, SystemTimePoint::clock::now());
        else
            stmt->setNull(6);
        // Only a draft waits for its time
        Optional<SystemTimePoint> publishAt = published ? Optional<SystemTimePoint>() : input.PublishAt.value_or(std::nullopt);
        if (publishAt)
            stmt->setDate(7, *publishAt);
        else
            stmt->setNull(7);
        trans->Append(stmt);

        for (auto const& [lang, translation] : input.Translations)
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_NEW_POST_TRANSLATION);
            stmt->setString(0, lang);
            stmt->setString(1, translation->Title);
            stmt->setString(2, translation->Summary);
            stmt->setString(3, translation->Body);
            trans->Append(stmt);
        }

        for (std::string const& tag : input.Tags.value_or(std::vector<std::string>()))
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_NEW_POST_TAG);
            stmt->setString(0, tag);
            trans->Append(stmt);
        }

        // The slug may have been an old one of another post
        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_REDIRECT);
        stmt->setString(0, *input.Slug);
        trans->Append(stmt);
        BlogDatabase.DirectCommitTransaction(trans);

        if (Optional<uint32> id = WorkerFindPostId(*input.Slug))
        {
            if (askReview)
                WorkerSetReview(*id, authorId);
            result.Post = WorkerLoadPost(*id, true);
        }
        if (result.Post)
        {
            WorkerUpdatePostWords(*result.Post);
            WorkerSyncPostRevisions(result.Post->Id, authorId);
        }
        result.Tags = WorkerLoadTags();
        return result;
    });
    if (result.SlugTaken)
        co_return request->RespondError(http::status::conflict, "conflict", {}, { { "slug", "taken" } });

    if (!result.Post)
        co_return RespondInternalError(request);

    RT_LOG_INFO("blog.admin", "{} created post {} ({})", result.Post->OwnerUsername, result.Post->Slug, result.Post->Id);
    sWorld->SetTags(std::move(result.Tags));
    if (result.Post->PublishAt)
        sWorld->WakeForScheduledPost(*result.Post->PublishAt);
    if (result.Post->IsPublished())
    {
        sWorld->RequestRebuild();
        sWorld->AnnouncePublished("post", "/posts/" + result.Post->Slug, OriginalTitle(*result.Post));
        sWorld->QueueNewsletter("post", result.Post->Id);
    }
    RespondPost(request, *result.Post, PostAccess{ .Owner = true }, http::status::created);
}

Rooftree::Task<void> AdminUpdatePost(Session& session, ApiRequest request)
{
    Admin::PostInput const& in = request->Get<Admin::PostInput>();
    flatbuffers::Optional<uint32> id = in.id();
    if (!id)
        co_return RespondNotFound(request);

    TableFields fields(request->GetErrors());
    Optional<PostInput> input = ReadPostInput(false, in, fields);
    if (!input)
        co_return RespondValidation(request, fields.TakeErrors());

    struct Result
    {
        bool NotFound = false;
        bool SlugTaken = false;
        std::string MissingOriginal;
        // What it changed and may not: the fields' names
        std::vector<std::string> NotAllowed;
        bool UnknownOwner = false;
        // A language's working copy that can't be published
        std::string Unpublishable;
        bool ReviewRequired = false;    // a time to publish it at, which a review can't keep
        bool WasPublished = false;
        bool PassagesChanged = false;
        PostAccess Access;
        Optional<PostRow> Post;
        std::vector<TagInfo> Tags;
    };

    // Published: with what its editors wrote
    std::vector<DocSnapshot> snapshots = sWorld->GetCollab().GetSnapshots(*id);
    bool replaced = !input->Translations.empty();
    bool review = NeedsReview(request->GetAuth().User);
    auto by = request->GetAuth().User.Username;
    Result result = co_await session.OnWorker([input = std::move(*input), id = *id, user = request->GetAuth().User, snapshots = std::move(snapshots), review]
    {
        Result result;
        Optional<PostAccess> access = WorkerPostAccess(id, user);
        Optional<PostRow> current = access && access->CanEdit() ? WorkerLoadPost(id, true) : Optional<PostRow>();
        if (!current)
        {
            result.NotFound = true;
            return result;
        }
        result.Access = *access;

        // What it sends as it was needs no right: a page sends the whole post
        auto changed = [](auto const& value, auto const& was) { return value && *value != was; };
        auto need = [&](std::string_view right, std::string_view field, bool change)
        {
            if (change && !access->Can(right))
                result.NotAllowed.emplace_back(field);
        };
        need(PostRightPublish, "status", changed(input.Status, current->Status));
        need(PostRightPublish, "publishAt", changed(input.PublishAt, current->PublishAt));
        need(PostRightSettings, "slug", changed(input.Slug, current->Slug));
        need(PostRightSettings, "originalLang", changed(input.OriginalLang, current->OriginalLang));
        need(PostRightSettings, "cover", changed(input.Cover, current->Cover));
        need(PostRightSettings, "commentsEnabled", changed(input.CommentsEnabled, current->CommentsEnabled));
        need(PostRightSettings, "tags", changed(input.Tags, current->Tags));
        // A language added is text; one taken away is a setting
        for (auto const& [lang, translation] : input.Translations)
            need(PostRightSettings, "translations." + lang, !translation && std::ranges::find(current->Langs, lang) != current->Langs.end());
        if (input.Owner && *input.Owner != current->OwnerUsername && !access->Admin)
            result.NotAllowed.emplace_back("owner");
        need(PostRightPublish, "reviewRequested", input.ReviewRequested.has_value());
        if (!result.NotAllowed.empty())
            return result;

        // The new owner: no longer an editor of it, the one before it is, with every right
        Optional<uint32> newOwner;
        if (input.Owner && *input.Owner != current->OwnerUsername)
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ACTIVE_USER_BY_NAME);
            stmt->setString(0, *input.Owner);
            if (PreparedQueryResult row = BlogDatabase.Query(stmt))
                newOwner = row->Fetch()[0].GetUInt32();
            else
            {
                result.UnknownOwner = true;
                return result;
            }
        }

        result.WasPublished = current->IsPublished();
        std::string slug = input.Slug.value_or(current->Slug);
        std::string status = input.Status.value_or(current->Status);
        // With reviews: published by the admin; asked for, it stays a draft until then
        bool askReview = review && status == "published" && !result.WasPublished;
        if (askReview)
            status = current->Status;
        if (review && input.PublishAt && *input.PublishAt && changed(input.PublishAt, current->PublishAt))
        {
            result.ReviewRequired = true;
            return result;
        }
        std::string originalLang = input.OriginalLang.value_or(current->OriginalLang);
        if (slug != current->Slug)
        {
            Optional<uint32> other = WorkerFindPostId(slug);
            if (other && *other != id)
            {
                result.SlugTaken = true;
                return result;
            }
        }

        // The languages after the update must still include the original one
        std::vector<std::string> langs = current->Langs;
        for (auto const& [lang, translation] : input.Translations)
        {
            std::erase(langs, lang);
            if (translation)
                langs.push_back(lang);
        }
        if (std::ranges::find(langs, originalLang) == langs.end())
        {
            result.MissingOriginal = originalLang;
            return result;
        }

        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_POST);
        stmt->setString(0, slug);
        stmt->setString(1, status);
        stmt->setString(2, originalLang);
        stmt->setString(3, input.Cover.value_or(current->Cover));
        stmt->setBool(4, input.CommentsEnabled.value_or(current->CommentsEnabled));
        if (status == "published")
            stmt->setDate(5, SystemTimePoint::clock::now());
        else
            stmt->setNull(5);
        // Only a draft waits for its time; a time that passed meanwhile is the server's to publish
        Optional<SystemTimePoint> publishAt = status == "published" ? Optional<SystemTimePoint>() : input.PublishAt.value_or(current->PublishAt);
        if (publishAt)
            stmt->setDate(6, *publishAt);
        else
            stmt->setNull(6);
        stmt->setUInt32(7, id);
        trans->Append(stmt);

        // Published now: its working copy is what readers get, but what this update sends
        if (status == "published" && !result.WasPublished)
        {
            std::set<std::string> sent;
            for (auto const& [lang, translation] : input.Translations)
                sent.insert(lang);
            if (Optional<std::string> field = WorkerApplyDrafts(id, snapshots, trans, sent))
            {
                result.Unpublishable = *field;
                return result;
            }
        }

        // A language's text sent, or deleted: its working copy with it
        for (auto const& [lang, translation] : input.Translations)
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_POST_DRAFT_LANG);
            stmt->setUInt32(0, id);
            stmt->setString(1, lang);
            trans->Append(stmt);
        }

        for (auto const& [lang, translation] : input.Translations)
        {
            if (translation)
            {
                stmt = BlogDatabase.GetPreparedStatement(BLOG_REP_POST_TRANSLATION);
                stmt->setUInt32(0, id);
                stmt->setString(1, lang);
                stmt->setString(2, translation->Title);
                stmt->setString(3, translation->Summary);
                stmt->setString(4, translation->Body);
            }
            else
            {
                stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_POST_TRANSLATION);
                stmt->setUInt32(0, id);
                stmt->setString(1, lang);
            }
            trans->Append(stmt);
        }

        if (input.Tags)
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_POST_TAGS);
            stmt->setUInt32(0, id);
            trans->Append(stmt);
            for (std::string const& tag : *input.Tags)
            {
                stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_POST_TAG);
                stmt->setUInt32(0, id);
                stmt->setString(1, tag);
                trans->Append(stmt);
            }
        }

        if (newOwner)
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_POST_OWNER);
            stmt->setUInt32(0, *newOwner);
            stmt->setUInt32(1, id);
            trans->Append(stmt);
            stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_POST_EDITOR);
            stmt->setUInt32(0, id);
            stmt->setUInt32(1, *newOwner);
            trans->Append(stmt);
            stmt = BlogDatabase.GetPreparedStatement(BLOG_REP_POST_EDITOR);
            stmt->setUInt32(0, id);
            stmt->setUInt32(1, current->OwnerId);
            stmt->setString(2, std::string("publish,settings,editors"));
            stmt->setUInt32(3, user.Id);
            trans->Append(stmt);
        }

        if (slug != current->Slug)
        {
            // Links to the old address keep working
            stmt = BlogDatabase.GetPreparedStatement(BLOG_REP_REDIRECT);
            stmt->setString(0, current->Slug);
            stmt->setUInt32(1, id);
            trans->Append(stmt);
            stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_REDIRECT);
            stmt->setString(0, slug);
            trans->Append(stmt);
        }

        BlogDatabase.DirectCommitTransaction(trans);
        // Asked to be published; published (by the admin), or taken back: not any more
        if (askReview)
            WorkerSetReview(id, user.Id);
        else if ((status == "published" && current->ReviewRequestedAt) || input.ReviewRequested)
            WorkerSetReview(id, {});
        result.Post = WorkerLoadPost(id, true);
        if (result.Post)
        {
            WorkerUpdatePostWords(*result.Post);
            result.PassagesChanged = WorkerSyncPostRevisions(result.Post->Id, user.Id);
        }
        result.Tags = WorkerLoadTags();
        return result;
    });
    if (result.NotFound)
        co_return RespondNotFound(request);
    if (!result.NotAllowed.empty())
    {
        boost::json::object errors;
        for (std::string const& field : result.NotAllowed)
            errors[field] = "not_allowed";
        co_return request->RespondError(http::status::forbidden, "not_allowed", {}, errors);
    }
    if (result.UnknownOwner)
        co_return RespondValidation(request, { { "owner", "not_found" } });
    if (!result.Unpublishable.empty())
        co_return RespondValidation(request, { { result.Unpublishable, "required" } });
    if (result.ReviewRequired)
        co_return RespondValidation(request, { { "publishAt", "review_required" } });
    if (result.SlugTaken)
        co_return request->RespondError(http::status::conflict, "conflict", {}, { { "slug", "taken" } });
    if (!result.MissingOriginal.empty())
        co_return RespondValidation(request, { { "translations." + result.MissingOriginal, "required" } });
    if (!result.Post)
        co_return RespondInternalError(request);

    RT_LOG_INFO("blog.admin", "{} updated post {} ({})", by, result.Post->Slug, result.Post->Id);
    // Its editors' pages start again from the text sent
    if (replaced)
        sWorld->GetCollab().Forget(result.Post->Id, "replaced");
    sWorld->SetTags(std::move(result.Tags));
    if (result.Post->PublishAt)
        sWorld->WakeForScheduledPost(*result.Post->PublishAt);
    if (result.WasPublished || result.Post->IsPublished())
    {
        sWorld->RequestRebuild();
        // Its open pages show what is published now, or that nothing is
        sWorld->AnnounceChanged(Rooftree::StringFormat("post:{}", result.Post->Id), "content");
        if (result.PassagesChanged)
            sWorld->AnnounceChanged(Rooftree::StringFormat("post:{}", result.Post->Id), "comments");
    }
    if (!result.WasPublished && result.Post->IsPublished())
    {
        sWorld->AnnouncePublished("post", "/posts/" + result.Post->Slug, OriginalTitle(*result.Post));
        // Once: a post mailed before isn't again (mailed_at)
        sWorld->QueueNewsletter("post", result.Post->Id);
    }
    RespondPost(request, *result.Post, result.Access);
}

// What its editors wrote, to readers: a draft published, a published post's working copy published
Rooftree::Task<void> AdminPublishPost(Session& session, ApiRequest request)
{
    flatbuffers::Optional<uint32> id = request->Get<Admin::IdRequest>().id();
    if (!id)
        co_return RespondNotFound(request);

    struct Result
    {
        bool NotFound = false;
        bool NotAllowed = false;
        std::string Unpublishable;
        bool WasPublished = false;
        bool PassagesChanged = false;
        bool ReviewRequested = false;
        PostAccess Access;
        Optional<PostRow> Post;
    };

    auto by = request->GetAuth().User.Username;
    Result result = co_await session.OnWorker([id = *id, user = request->GetAuth().User, snapshots = sWorld->GetCollab().GetSnapshots(*id),
        review = NeedsReview(request->GetAuth().User)]
    {
        Result result;
        Optional<PostAccess> access = WorkerPostAccess(id, user);
        Optional<PostRow> current = access && access->CanEdit() ? WorkerLoadPost(id, false) : Optional<PostRow>();
        if (!current)
        {
            result.NotFound = true;
            return result;
        }
        if (!access->Can(PostRightPublish))
        {
            result.NotAllowed = true;
            return result;
        }
        result.Access = *access;
        result.WasPublished = current->IsPublished();

        // With reviews: the working copy as it is, the admin asked to publish it
        if (review)
        {
            WorkerWriteSnapshots(id, snapshots);
            WorkerSetReview(id, user.Id);
            result.ReviewRequested = true;
            result.Post = WorkerLoadPost(id, true);
            return result;
        }

        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
        if (Optional<std::string> field = WorkerApplyDrafts(id, snapshots, trans))
        {
            result.Unpublishable = *field;
            return result;
        }
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_POST_PUBLISH);
        stmt->setUInt32(0, id);
        trans->Append(stmt);
        BlogDatabase.DirectCommitTransaction(trans);

        result.Post = WorkerLoadPost(id, true);
        if (result.Post)
        {
            WorkerUpdatePostWords(*result.Post);
            result.PassagesChanged = WorkerSyncPostRevisions(result.Post->Id, user.Id);
        }
        return result;
    });
    if (result.NotFound)
        co_return RespondNotFound(request);
    if (result.NotAllowed)
        co_return request->RespondError(http::status::forbidden, "not_allowed");
    if (!result.Unpublishable.empty())
        co_return RespondValidation(request, { { result.Unpublishable, "required" } });
    if (!result.Post)
        co_return RespondInternalError(request);
    if (result.ReviewRequested)
    {
        RT_LOG_INFO("blog.admin", "{} asked for post {} ({}) to be published", by, result.Post->Slug, result.Post->Id);
        co_return RespondPost(request, *result.Post, result.Access, http::status::accepted);
    }

    RT_LOG_INFO("blog.admin", "{} published post {} ({})", by, result.Post->Slug, result.Post->Id);
    sWorld->RequestRebuild();
    sWorld->AnnounceChanged(Rooftree::StringFormat("post:{}", result.Post->Id), "content");
    if (result.PassagesChanged)
        sWorld->AnnounceChanged(Rooftree::StringFormat("post:{}", result.Post->Id), "comments");
    if (!result.WasPublished)
    {
        sWorld->AnnouncePublished("post", "/posts/" + result.Post->Slug, OriginalTitle(*result.Post));
        sWorld->QueueNewsletter("post", result.Post->Id);
    }
    RespondPost(request, *result.Post, result.Access);
}

namespace
{
constexpr uint32 RevisionsPageSize = 50;

// The first 11 fields of BLOG_SEL_POST_REVISIONS and BLOG_SEL_POST_REVISION
struct RevisionRow
{
    uint32 Id = 0;
    std::string Lang, Kind, Title, Note;
    SystemTimePoint CreatedAt;
    uint32 Length = 0;
    Optional<uint32> AuthorId;
    std::string Username, DisplayName, Avatar;

    static RevisionRow FromFields(Field const* fields)
    {
        return { .Id = fields[0].GetUInt32(), .Lang = fields[1].GetString(), .Kind = fields[2].GetString(), .Title = fields[4].GetString(),
            .Note = fields[5].GetString(), .CreatedAt = fields[3].GetDate(), .Length = uint32(fields[6].GetInt64()),
            .AuthorId = fields[7].GetUInt32OrNull(), .Username = fields[8].GetStringOrNull().value_or(""),
            .DisplayName = fields[9].GetStringOrNull().value_or(""), .Avatar = fields[10].GetStringOrNull().value_or("") };
    }

    flatbuffers::Offset<Admin::PostRevision> Build(flatbuffers::FlatBufferBuilder& builder, std::string const* body = nullptr,
        std::string const* summary = nullptr, flatbuffers::Offset<Admin::PostRevisionText> previous = 0) const
    {
        auto author = AuthorId ? PostPersonTable(builder, *AuthorId, Username, DisplayName, Avatar) : 0;
        return Admin::CreatePostRevision(builder, Id, builder.CreateString(Lang), TimeString(builder, CreatedAt), builder.CreateString(Title),
            body ? builder.CreateString(*body) : 0, builder.CreateString(Kind), builder.CreateString(Note), Length, author,
            summary ? builder.CreateString(*summary) : 0, previous);
    }
};

// Worker: the versions of a post, newest first, a page before before (0: from the newest)
std::string WorkerRevisionsAnswer(uint32 postId, uint32 before, std::vector<uint32> const& only = {})
{
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::PostRevision>> revisions;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_REVISIONS);
    stmt->setUInt32(0, postId);
    stmt->setUInt32(1, before);
    stmt->setUInt32(2, before);
    stmt->setUInt32(3, RevisionsPageSize + 1);
    bool hasMore = false;
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        std::size_t count = 0;
        do
        {
            if (++count > RevisionsPageSize)
            {
                hasMore = true;
                break;
            }
            RevisionRow row = RevisionRow::FromFields(rows->Fetch());
            if (only.empty() || std::ranges::find(only, row.Id) != only.end())
                revisions.push_back(row.Build(builder));
        } while (rows->NextRow());
    }
    builder.Finish(Admin::CreatePostRevisions(builder, builder.CreateVector(revisions), hasMore && only.empty()));
    return Bytes(builder);
}

}

Rooftree::Task<void> AdminPostRevisions(Session& session, ApiRequest request)
{
    Admin::PostRevisionsRequest const& in = request->Get<Admin::PostRevisionsRequest>();
    if (!in.id())
        co_return RespondNotFound(request);

    Optional<std::string> payload = co_await session.OnWorker([id = *in.id(), before = in.before().value_or(0), user = request->GetAuth().User]() -> Optional<std::string>
    {
        Optional<PostAccess> access = WorkerPostAccess(id, user);
        if (!access || !access->CanEdit())
            return {};
        return WorkerRevisionsAnswer(id, before);
    });
    if (!payload)
        co_return RespondNotFound(request);
    request->Respond(std::move(*payload));
}

// One version in full, with the one before it in its language: what it changed
Rooftree::Task<void> AdminPostRevision(Session& session, ApiRequest request)
{
    Admin::PostRevisionRequest const& in = request->Get<Admin::PostRevisionRequest>();
    if (!in.id() || !in.revision())
        co_return RespondNotFound(request);

    Optional<std::string> payload = co_await session.OnWorker([id = *in.id(), revision = *in.revision(), user = request->GetAuth().User]() -> Optional<std::string>
    {
        Optional<PostAccess> access = WorkerPostAccess(id, user);
        if (!access || !access->CanEdit())
            return {};

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_REVISION);
        stmt->setUInt32(0, id);
        stmt->setUInt32(1, revision);
        PreparedQueryResult result = BlogDatabase.Query(stmt);
        if (!result)
            return {};
        Field* fields = result->Fetch();
        RevisionRow row = RevisionRow::FromFields(fields);
        std::string summary = fields[11].GetString();
        std::string body = fields[12].GetString();

        flatbuffers::FlatBufferBuilder builder;
        flatbuffers::Offset<Admin::PostRevisionText> previous;
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_REVISION_BEFORE);
        stmt->setUInt32(0, id);
        stmt->setString(1, row.Lang);
        stmt->setUInt32(2, row.Id);
        if (PreparedQueryResult before = BlogDatabase.Query(stmt))
            previous = Admin::CreatePostRevisionTextDirect(builder, before->Fetch()[0].GetString().c_str(), before->Fetch()[1].GetString().c_str(),
                before->Fetch()[2].GetString().c_str());
        builder.Finish(row.Build(builder, &body, &summary, previous));
        return Bytes(builder);
    });
    if (!payload)
        co_return RespondNotFound(request);
    request->Respond(std::move(*payload));
}

// The working copy saved as a version of each language it changed since that language's last
// version: what an editor wants to be able to go back to. Answers the versions it made
Rooftree::Task<void> AdminSavePostVersion(Session& session, ApiRequest request)
{
    Admin::PostSaveVersionRequest const& in = request->Get<Admin::PostSaveVersionRequest>();
    if (!in.id())
        co_return RespondNotFound(request);
    TableFields fields(request->GetErrors());
    std::string note = fields.Text("note", in.note(), 0, 200, false);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    auto id = *in.id();
    auto by = request->GetAuth().User.Username;
    Optional<std::pair<std::string, std::size_t>> result = co_await session.OnWorker([id = *in.id(), note, user = request->GetAuth().User, snapshots = sWorld->GetCollab().GetSnapshots(*in.id())]()
        -> Optional<std::pair<std::string, std::size_t>>
    {
        Optional<PostAccess> access = WorkerPostAccess(id, user);
        if (!access || !access->CanEdit())
            return {};

        WorkerWriteSnapshots(id, snapshots);
        std::vector<uint32> saved;
        for (std::string lang : { "zh", "en" })
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_DOC_TEXTS);
            stmt->setString(0, lang);
            stmt->setString(1, lang);
            stmt->setUInt32(2, id);
            PreparedQueryResult text = BlogDatabase.Query(stmt);
            if (!text)
                continue;
            Field* row = text->Fetch();
            // A language neither published nor begun
            if (row[0].IsNull() && row[4].IsNull())
                continue;
            std::string title = row[0].GetStringOrNull().value_or("");
            std::string summary = row[2].GetStringOrNull().value_or("");
            std::string body = row[4].GetStringOrNull().value_or("");

            stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_LATEST_REVISION_TEXT);
            stmt->setUInt32(0, id);
            stmt->setString(1, lang);
            stmt->setString(2, std::string());
            stmt->setString(3, std::string());
            if (PreparedQueryResult latest = BlogDatabase.Query(stmt))
                if (latest->Fetch()[0].GetString() == title && latest->Fetch()[1].GetString() == body && latest->Fetch()[2].GetString() == summary)
                    continue;

            stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_POST_REVISION);
            stmt->setUInt32(0, id);
            stmt->setString(1, lang);
            stmt->setString(2, std::string("draft"));
            stmt->setString(3, title);
            stmt->setString(4, summary);
            stmt->setString(5, body);
            stmt->setUInt32(6, user.Id);
            stmt->setString(7, note);
            BlogDatabase.DirectExecute(stmt);
            // Its id: the post's newest version, just made
            BlogDatabasePreparedStatement* newest = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_REVISIONS);
            newest->setUInt32(0, id);
            newest->setUInt32(1, 0);
            newest->setUInt32(2, 0);
            newest->setUInt32(3, 1);
            if (PreparedQueryResult made = BlogDatabase.Query(newest))
                saved.push_back(made->Fetch()[0].GetUInt32());
        }
        if (saved.empty())
            // Nothing changed since: no version, an empty list (no version has id 0)
            return std::make_pair(WorkerRevisionsAnswer(id, 0, { 0 }), std::size_t(0));
        return std::make_pair(WorkerRevisionsAnswer(id, 0, saved), saved.size());
    });
    if (!result)
        co_return RespondNotFound(request);
    if (result->second)
        RT_LOG_INFO("blog.admin", "{} saved {} version(s) of post {}", by, result->second, id);
    request->Respond(std::move(result->first), result->second ? http::status::created : http::status::ok);
}

// A version back in the working copy of its language: its editors' pages open it again, and go on
// from it; readers get it once someone publishes it
Rooftree::Task<void> AdminRestorePostRevision(Session& session, ApiRequest request)
{
    Admin::PostRevisionRequest const& in = request->Get<Admin::PostRevisionRequest>();
    if (!in.id() || !in.revision())
        co_return RespondNotFound(request);

    struct Result
    {
        PostAccess Access;
        Optional<PostRow> Post;
        std::string Lang;
    };

    auto revision = *in.revision();
    auto by = request->GetAuth().User.Username;
    Result result = co_await session.OnWorker([id = *in.id(), revision = *in.revision(), user = request->GetAuth().User, snapshots = sWorld->GetCollab().GetSnapshots(*in.id())]
    {
        Result result;
        Optional<PostAccess> access = WorkerPostAccess(id, user);
        if (!access || !access->CanEdit())
            return result;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_REVISION);
        stmt->setUInt32(0, id);
        stmt->setUInt32(1, revision);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
            return result;
        Field* fields = row->Fetch();
        result.Lang = fields[1].GetString();

        // Its documents' versions first: the restored text is newer than all of them
        WorkerWriteSnapshots(id, snapshots);
        stmt = BlogDatabase.GetPreparedStatement(BLOG_UPS_POST_DRAFT_RESTORE);
        stmt->setUInt32(0, id);
        stmt->setString(1, result.Lang);
        stmt->setString(2, fields[4].GetString());
        stmt->setString(3, fields[11].GetString());
        stmt->setString(4, fields[12].GetString());
        BlogDatabase.DirectExecute(stmt);
        result.Access = *access;
        result.Post = WorkerLoadPost(id, true);
        return result;
    });
    if (!result.Post)
        co_return RespondNotFound(request);

    RT_LOG_INFO("blog.admin", "{} restored version {} of post {} ({}) in {}", by, revision, result.Post->Slug, result.Post->Id, result.Lang);
    sWorld->GetCollab().Forget(result.Post->Id, "replaced");
    RespondPost(request, *result.Post, result.Access);
}

Rooftree::Task<void> AdminDeletePost(Session& session, ApiRequest request)
{
    flatbuffers::Optional<uint32> id = request->Get<Admin::IdRequest>().id();
    if (!id)
        co_return RespondNotFound(request);

    struct Result
    {
        bool NotAllowed = false;
        Optional<PostRow> Post;
        std::vector<TagInfo> Tags;
    };

    auto by = request->GetAuth().User.Username;
    Result result = co_await session.OnWorker([id = *id, user = request->GetAuth().User]
    {
        Result result;
        Optional<PostAccess> access = WorkerPostAccess(id, user);
        if (!access || !access->CanEdit())
            return result;
        // An editor's is its owner's to delete
        if (!access->CanDelete())
        {
            result.NotAllowed = true;
            return result;
        }
        result.Post = WorkerLoadPost(id, false);
        if (!result.Post)
            return result;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_POST);
        stmt->setUInt32(0, id);
        BlogDatabase.DirectExecute(stmt);
        result.Tags = WorkerLoadTags();
        return result;
    });
    if (result.NotAllowed)
        co_return request->RespondError(http::status::forbidden, "not_allowed");
    if (!result.Post)
        co_return RespondNotFound(request);

    RT_LOG_INFO("blog.admin", "{} deleted post {} ({})", by, result.Post->Slug, result.Post->Id);
    sWorld->GetCollab().Forget(result.Post->Id, "deleted");
    sWorld->SetTags(std::move(result.Tags));
    if (result.Post->IsPublished())
    {
        sWorld->RequestRebuild();
        sWorld->AnnounceChanged(Rooftree::StringFormat("post:{}", result.Post->Id), "content");
    }
    RespondOk(request);
}

namespace
{
struct EditorRow
{
    uint32 Id;
    std::string Username, DisplayName, Avatar;
    std::vector<std::string> Rights;
    SystemTimePoint CreatedAt;
};

struct EditorsResult
{
    bool NotFound = false;
    bool NotAllowed = false;
    bool UnknownUser = false;
    bool IsOwner = false;           // the one invited owns it
    uint32 EditorId = 0;            // the one invited or removed: its login knows it edits now, or not
    Optional<PostRow> Post;
    std::vector<EditorRow> Editors;
};

std::vector<EditorRow> WorkerLoadEditors(uint32 postId)
{
    std::vector<EditorRow> editors;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_EDITORS);
    stmt->setUInt32(0, postId);
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
        {
            Field* fields = rows->Fetch();
            editors.push_back({ .Id = fields[0].GetUInt32(), .Username = fields[1].GetString(), .DisplayName = fields[2].GetString(),
                .Avatar = fields[3].GetStringOrNull().value_or(""), .Rights = SplitList(fields[4].GetString()), .CreatedAt = fields[5].GetDate() });
        } while (rows->NextRow());
    }
    return editors;
}

// The answer of every editors operation: the post's owner and its editors
void RespondEditors(ApiRequest const& request, EditorsResult const& result)
{
    if (result.EditorId)
    {
        sWorld->ForgetUserAuth(result.EditorId);
        // Its page shows who wrote it
        if (result.Post && result.Post->IsPublished())
        {
            sWorld->RequestRebuild();
            sWorld->AnnounceChanged(Rooftree::StringFormat("post:{}", result.Post->Id), "content");
        }
    }
    if (result.NotFound || !result.Post)
        return RespondNotFound(request);
    if (result.NotAllowed)
        return request->RespondError(http::status::forbidden, "not_allowed");
    if (result.UnknownUser)
        return RespondValidation(request, { { "username", "not_found" } });
    if (result.IsOwner)
        return RespondValidation(request, { { "username", "owner" } });

    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::PostEditor>> editors;
    for (EditorRow const& editor : result.Editors)
        editors.push_back(Admin::CreatePostEditor(builder, PostPersonTable(builder, editor.Id, editor.Username, editor.DisplayName, editor.Avatar),
            StringList(builder, editor.Rights), TimeString(builder, editor.CreatedAt)));
    PostRow const& post = *result.Post;
    builder.Finish(Admin::CreatePostEditors(builder, PostPersonTable(builder, post.OwnerId, post.OwnerUsername, post.OwnerDisplayName, post.OwnerAvatar),
        builder.CreateVector(editors)));
    request->Respond(builder);
}

// Worker: the post and its editors if user edits it, else NotFound
EditorsResult WorkerEditorsOf(uint32 postId, UserRow const& user, Optional<PostAccess>& access)
{
    EditorsResult result;
    access = WorkerPostAccess(postId, user);
    if (!access || !access->CanEdit())
    {
        result.NotFound = true;
        return result;
    }
    result.Post = WorkerLoadPost(postId, false);
    return result;
}

// An editor with the editors right changes only who has no more rights than it: never the rights it lacks
bool MayManage(PostAccess const& access, std::vector<std::string> const& rights)
{
    if (!access.Can(PostRightEditors))
        return false;
    return std::ranges::all_of(rights, [&](std::string const& right) { return access.Can(right); });
}
}

Rooftree::Task<void> AdminPostEditors(Session& session, ApiRequest request)
{
    flatbuffers::Optional<uint32> id = request->Get<Admin::IdRequest>().id();
    if (!id)
        co_return RespondNotFound(request);

    EditorsResult result = co_await session.OnWorker([id = *id, user = request->GetAuth().User]
    {
        Optional<PostAccess> access;
        EditorsResult result = WorkerEditorsOf(id, user, access);
        if (result.Post)
            result.Editors = WorkerLoadEditors(id);
        return result;
    });
    RespondEditors(request, result);
}

Rooftree::Task<void> AdminSetPostEditor(Session& session, ApiRequest request)
{
    Admin::PostEditorInput const& in = request->Get<Admin::PostEditorInput>();
    if (!in.id())
        co_return RespondNotFound(request);

    TableFields fields(request->GetErrors());
    std::string username = fields.Text("username", in.username(), 1, 100);
    std::vector<std::string> rights;
    if (in.rights())
        for (flatbuffers::String const* right : *in.rights())
        {
            if (std::ranges::find(PostRights, right->string_view()) == std::end(PostRights))
                fields.AddError("rights", "invalid");
            else if (std::ranges::find(rights, right->str()) == rights.end())
                rights.push_back(right->str());
        }
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    auto by = request->GetAuth().User.Username;
    EditorsResult result = co_await session.OnWorker([id = *in.id(), user = request->GetAuth().User, username, rights]
    {
        Optional<PostAccess> access;
        EditorsResult result = WorkerEditorsOf(id, user, access);
        if (!result.Post)
            return result;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ACTIVE_USER_BY_NAME);
        stmt->setString(0, username);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
        {
            result.UnknownUser = true;
            return result;
        }
        uint32 editorId = row->Fetch()[0].GetUInt32();
        if (editorId == result.Post->OwnerId)
        {
            result.IsOwner = true;
            return result;
        }

        // The rights it has now count too: what it can't give, it can't take
        std::vector<std::string> before;
        for (EditorRow const& editor : WorkerLoadEditors(id))
            if (editor.Id == editorId)
                before = editor.Rights;
        if (!MayManage(*access, rights) || !MayManage(*access, before))
        {
            result.NotAllowed = true;
            return result;
        }

        result.EditorId = editorId;
        stmt = BlogDatabase.GetPreparedStatement(BLOG_REP_POST_EDITOR);
        stmt->setUInt32(0, id);
        stmt->setUInt32(1, editorId);
        stmt->setString(2, JoinList(rights));
        stmt->setUInt32(3, user.Id);
        BlogDatabase.DirectExecute(stmt);
        result.Editors = WorkerLoadEditors(id);
        return result;
    });
    if (result.Post && !result.NotAllowed && !result.UnknownUser && !result.IsOwner)
        RT_LOG_INFO("blog.admin", "{} made {} an editor of post {} ({}): {}", by, username, result.Post->Slug, result.Post->Id,
            rights.empty() ? std::string("its text") : JoinList(rights));
    RespondEditors(request, result);
}

Rooftree::Task<void> AdminRemovePostEditor(Session& session, ApiRequest request)
{
    Admin::PostEditorRemoveRequest const& in = request->Get<Admin::PostEditorRemoveRequest>();
    if (!in.id() || !in.user())
        co_return RespondNotFound(request);

    auto by = request->GetAuth().User.Username;
    auto editorId = *in.user();
    EditorsResult result = co_await session.OnWorker([id = *in.id(), editorId = *in.user(), user = request->GetAuth().User]
    {
        Optional<PostAccess> access;
        EditorsResult result = WorkerEditorsOf(id, user, access);
        if (!result.Post)
            return result;

        std::vector<EditorRow> editors = WorkerLoadEditors(id);
        auto editor = std::ranges::find(editors, editorId, &EditorRow::Id);
        if (editor == editors.end())
        {
            result.NotFound = true;
            return result;
        }
        // Leaving needs no right
        if (editorId != user.Id && !MayManage(*access, editor->Rights))
        {
            result.NotAllowed = true;
            return result;
        }

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_POST_EDITOR);
        stmt->setUInt32(0, id);
        stmt->setUInt32(1, editorId);
        BlogDatabase.DirectExecute(stmt);
        result.EditorId = editorId;
        result.Editors = WorkerLoadEditors(id);
        return result;
    });
    if (result.Post && !result.NotFound && !result.NotAllowed)
    {
        RT_LOG_INFO("blog.admin", "{} removed editor {} of post {} ({})", by, editorId, result.Post->Slug, result.Post->Id);
        sWorld->GetCollab().Revoke(result.Post->Id, editorId);
    }
    RespondEditors(request, result);
}

Rooftree::Task<void> AdminCreateTag(Session& session, ApiRequest request)
{
    Admin::TagInput const& in = request->Get<Admin::TagInput>();
    TableFields fields(request->GetErrors());
    std::string slug = fields.Slug("slug", in.slug(), MaxTagSlugLength);
    std::string nameZh = fields.Text("nameZh", in.nameZh(), 1, 50);
    std::string nameEn = fields.Text("nameEn", in.nameEn(), 1, 50);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    if (sWorld->FindTag(slug))
        co_return request->RespondError(http::status::conflict, "conflict", {}, { { "slug", "taken" } });

    std::vector<TagInfo> tags = co_await session.OnWorker([slug, nameZh, nameEn]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_TAG);
        stmt->setString(0, slug);
        stmt->setString(1, nameZh);
        stmt->setString(2, nameEn);
        BlogDatabase.DirectExecute(stmt);
        return WorkerLoadTags();
    });
    sWorld->SetTags(std::move(tags));
    TagInfo const* tag = sWorld->FindTag(slug);
    if (!tag)
        co_return RespondInternalError(request);

    RespondTag(request, *tag, http::status::created);
}

Rooftree::Task<void> AdminUpdateTag(Session& session, ApiRequest request)
{
    Admin::TagInput const& in = request->Get<Admin::TagInput>();
    std::string slug = in.slug() ? in.slug()->str() : std::string();
    TagInfo const* tag = sWorld->FindTag(slug);
    if (!tag)
        co_return RespondNotFound(request);

    TableFields fields(request->GetErrors());
    std::string nameZh = in.nameZh() ? fields.Text("nameZh", in.nameZh(), 1, 50) : tag->NameZh;
    std::string nameEn = in.nameEn() ? fields.Text("nameEn", in.nameEn(), 1, 50) : tag->NameEn;
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    std::vector<TagInfo> tags = co_await session.OnWorker([slug, nameZh, nameEn]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_TAG);
        stmt->setString(0, nameZh);
        stmt->setString(1, nameEn);
        stmt->setString(2, slug);
        BlogDatabase.DirectExecute(stmt);
        return WorkerLoadTags();
    });
    sWorld->SetTags(std::move(tags));
    TagInfo const* updated = sWorld->FindTag(slug);
    if (!updated)
        co_return RespondNotFound(request);

    if (updated->PostCount)
        sWorld->RequestRebuild();
    RespondTag(request, *updated);
}

Rooftree::Task<void> AdminDeleteTag(Session& session, ApiRequest request)
{
    flatbuffers::String const* slugValue = request->Get<Admin::SlugRequest>().slug();
    std::string slug = slugValue ? slugValue->str() : std::string();
    TagInfo const* tag = sWorld->FindTag(slug);
    if (!tag)
        co_return RespondNotFound(request);

    bool used = tag->PostCount > 0;
    std::vector<TagInfo> tags = co_await session.OnWorker([slug]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_TAG);
        stmt->setString(0, slug);
        BlogDatabase.DirectExecute(stmt);
        return WorkerLoadTags();
    });
    sWorld->SetTags(std::move(tags));
    if (used)
        sWorld->RequestRebuild();
    RespondOk(request);
}

namespace
{
constexpr uint32 SearchResults = 20;
constexpr uint32 FeedItems = 20;

std::string EscapeXml(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text)
    {
        switch (c)
        {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

// RFC 822, what RSS wants
std::string FormatRssTime(SystemTimePoint time)
{
    return Rooftree::StringFormat("{:%a, %d %b %Y %H:%M:%S} +0000", std::chrono::time_point_cast<std::chrono::seconds>(time));
}

void RespondXml(BlogSession::Request const& request, std::string_view contentType, std::string body)
{
    Rooftree::Net::Http::Response& response = request->GetResponse();
    response.result(http::status::ok);
    response.set(http::field::content_type, contentType);
    response.set(http::field::cache_control, "public, max-age=600");
    response.body() = std::move(body);
    request->Respond();
}
}

Rooftree::Task<void> SearchPosts(Session& /*session*/, ApiRequest request)
{
    Reader::PostsSearchRequest const& in = request->Get<Reader::PostsSearchRequest>();
    std::string lang = ReadLang(in.lang());
    // In the page's language; "all" in every one, each result in the version found
    bool all = in.all() && in.all()->string_view() == "1";
    std::string query = ReadText(in.q());
    query.erase(0, query.find_first_not_of(" \t"));
    query.erase(query.find_last_not_of(" \t") + 1);
    std::size_t length = CountCharacters(query);
    if (!SearchIndex::IsSearchable(query) || length > 100 || !IsCleanText(query, false))
        co_return RespondValidation(request, { { "q", length > 100 ? "too_long" : IsCleanText(query, false) ? "too_short" : "invalid" } });

    // In memory, here on the main thread: no database (BlogWorld.h, SearchIndex.h)
    sWorld->WithSearchIndex([request, lang, all, query](SearchIndex const& index)
    {
        std::vector<SearchIndex::Hit> found = index.Search(query, all ? std::string_view() : std::string_view(lang), SearchResults);

        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<Reader::PostSummary>> posts;
        for (SearchIndex::Hit const& hit : found)
        {
            SearchPost const& post = *index.FindPost(hit.Id);
            SearchText const& text = hit.Text ? *hit.Text : post.GetText(lang);
            PostSummary summary{ post.Id, post.Slug, post.Cover, post.PublishedAt, post.UpdatedAt, post.OriginalLang, text.Lang, text.Title,
                text.Summary, post.Tags, {}, sWorld->GetCommentCount(post.Id) };
            for (SearchText const& version : post.Texts)
                summary.Langs.push_back(version.Lang);
            posts.push_back(PostSummaryTable(builder, summary, lang));
        }

        builder.Finish(Reader::CreatePostsSearch(builder, builder.CreateVector(posts)));
        request->Respond(builder);
    });
}

// Posts, newest first
Rooftree::Task<void> BlogSession::HandleFeed(Request request)
{
    // /zh/feed.xml, or /api/feed.xml?lang=zh
    std::string_view pathLang = request->GetPathParameter("lang");
    if (!pathLang.empty() && !IsValidLang(pathLang))
        co_return RespondNotFound(request);

    std::string lang = pathLang.empty() ? GetLang(request) : std::string(pathLang);
    struct FeedItem
    {
        std::string Path;
        std::string Title;
        std::string Summary;
        SystemTimePoint PublishedAt;
        std::vector<std::string> Tags;
    };

    std::vector<FeedItem> posts = co_await OnWorker([lang]
    {
        std::vector<PostSummary> posts;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POSTS);
        stmt->setString(0, lang);
        stmt->setString(1, std::string_view());
        stmt->setString(2, std::string_view());
        stmt->setDate(3, PermanentBanTime());
        stmt->setDate(4, PermanentBanTime());
        stmt->setUInt32(5, std::numeric_limits<uint32>::max());
        stmt->setUInt32(6, FeedItems);
        if (PreparedQueryResult result = BlogDatabase.Query(stmt))
        {
            do
                posts.push_back(ReadPostSummary(result->Fetch()));
            while (result->NextRow());
        }

        std::vector<FeedItem> items;
        for (PostSummary& post : posts)
            items.push_back({ "/posts/" + post.Slug, std::move(post.Title), std::move(post.Summary), post.PublishedAt, std::move(post.Tags) });
        return items;
    });
    BlogSettings const& settings = sWorld->GetSettings();
    std::string home = settings.PublicOrigin + "/" + lang;
    std::string xml = Rooftree::StringFormat(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rss version=\"2.0\" xmlns:atom=\"http://www.w3.org/2005/Atom\">\n<channel>\n"
        "<title>{}</title>\n<link>{}</link>\n<description>{}</description>\n<language>{}</language>\n"
        "<atom:link href=\"{}/feed.xml\" rel=\"self\" type=\"application/rss+xml\"/>\n",
        EscapeXml(sWorld->GetSiteName(lang)), EscapeXml(home), EscapeXml(sWorld->GetSiteName(lang)), lang == "en" ? "en" : "zh-CN", EscapeXml(home));
    if (!posts.empty())
        xml += "<lastBuildDate>" + FormatRssTime(posts.front().PublishedAt) + "</lastBuildDate>\n";

    for (FeedItem const& post : posts)
    {
        std::string link = EscapeXml(home + post.Path);
        xml += Rooftree::StringFormat("<item>\n<title>{}</title>\n<link>{}</link>\n<guid isPermaLink=\"true\">{}</guid>\n<pubDate>{}</pubDate>\n",
            EscapeXml(post.Title), link, link, FormatRssTime(post.PublishedAt));
        if (!post.Summary.empty())
            xml += "<description>" + EscapeXml(post.Summary) + "</description>\n";
        for (std::string const& tag : post.Tags)
            if (TagInfo const* info = sWorld->FindTag(tag))
                xml += "<category>" + EscapeXml(info->GetName(lang)) + "</category>\n";
        xml += "</item>\n";
    }

    xml += "</channel>\n</rss>\n";
    RespondXml(request, "application/rss+xml; charset=utf-8", std::move(xml));
}

Rooftree::Task<void> BlogSession::HandleSitemap(Request request)
{
    struct Entry
    {
        std::string Slug;
        SystemTimePoint UpdatedAt;
        std::vector<std::string> Langs;
    };

    struct Result
    {
        std::vector<Entry> Posts;
    };

    Result result = co_await OnWorker([]
    {
        Result result;
        if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_PUBLISHED_SLUGS)))
        {
            do
            {
                Field* fields = rows->Fetch();
                result.Posts.push_back({ fields[0].GetString(), fields[1].GetDate(), SplitList(fields[2].GetStringOrNull().value_or("")) });
            } while (rows->NextRow());
        }
        return result;
    });
    std::vector<Entry> const& entries = result.Posts;
    std::string const& origin = sWorld->GetSettings().PublicOrigin;
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\" xmlns:xhtml=\"http://www.w3.org/1999/xhtml\">\n";

    // Every page in each of its languages, each listing the others as alternates
    auto add = [&](std::string const& path, std::vector<std::string> const& langs, Optional<SystemTimePoint> modified)
    {
        for (std::string const& lang : langs)
        {
            xml += "<url><loc>" + EscapeXml(origin + "/" + lang + path) + "</loc>";
            if (modified)
                xml += Rooftree::StringFormat("<lastmod>{:%Y-%m-%d}</lastmod>", std::chrono::time_point_cast<std::chrono::days>(*modified));
            for (std::string const& other : langs)
                xml += "<xhtml:link rel=\"alternate\" hreflang=\"" + other + "\" href=\"" + EscapeXml(origin + "/" + other + path) + "\"/>";
            xml += "</url>\n";
        }
    };

    std::vector<std::string> bothLangs{ "zh", "en" };
    add("", bothLangs, entries.empty() ? Optional<SystemTimePoint>() : entries.front().UpdatedAt);
    for (Entry const& entry : entries)
        add("/posts/" + entry.Slug, entry.Langs, entry.UpdatedAt);
    for (auto const& [slug, tag] : sWorld->GetTags())
        if (tag.PostCount)
            add("/tags/" + slug, TagLangs(tag), {});
    for (std::string_view page : { "/posts", "/timeline", "/friends", "/projects", "/about", "/guestbook", "/rss", "/sitemap" })
        add(std::string(page), bothLangs, {});

    xml += "</urlset>\n";
    RespondXml(request, "application/xml; charset=utf-8", std::move(xml));
}

// For crawlers: everything but the admin's, the account's and the status page (logged in only), and where the sitemap is
void BlogSession::HandleRobots(Request const& request)
{
    std::string text = "User-agent: *\n";
    for (std::string_view lang : { "zh", "en" })
        for (std::string_view page : { "/admin", "/account", "/status" })
            text += Rooftree::StringFormat("Disallow: /{}{}\n", lang, page);
    text += "\nSitemap: " + sWorld->GetSettings().PublicOrigin + "/sitemap.xml\n";
    RespondXml(request, "text/plain; charset=utf-8", std::move(text));
}

std::vector<TagInfo> LoadTagsDirect()
{
    return WorkerLoadTags();
}

// Rows from before word_count: counted once, at startup
void CountMissingWordsDirect()
{
    PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_POSTS_WITHOUT_WORDS));
    if (!rows)
        return;

    do
    {
        Field* fields = rows->Fetch();
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_POST_WORDS);
        stmt->setUInt32(0, uint32(CountWords(fields[1].GetString()) + CountWords(fields[2].GetString())));
        stmt->setUInt32(1, fields[0].GetUInt32());
        BlogDatabase.DirectExecute(stmt);
    } while (rows->NextRow());
}

std::vector<ScheduledPost> PublishDuePostsDirect()
{
    std::vector<ScheduledPost> due;
    if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_DUE_POSTS)))
    {
        do
        {
            Field* fields = rows->Fetch();
            due.push_back({ fields[0].GetUInt32(), fields[1].GetString(), fields[2].GetString() });
        } while (rows->NextRow());
    }
    for (ScheduledPost& post : due)
    {
        // With its working copy as last written (a second at most behind its editors)
        BlogDatabaseTransaction trans = BlogDatabase.BeginTransaction();
        if (Optional<std::string> field = WorkerApplyDrafts(post.Id, {}, trans))
        {
            RT_LOG_WARN("blog.admin", "Post {} is published as it was: {} of its working copy is empty", post.Slug, *field);
            trans = BlogDatabase.BeginTransaction();
        }
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_PUBLISH_DUE_POST);
        stmt->setUInt32(0, post.Id);
        trans->Append(stmt);
        BlogDatabase.DirectCommitTransaction(trans);
        post.PassagesChanged = WorkerSyncPostRevisions(post.Id, {});
    }
    return due;
}
}
