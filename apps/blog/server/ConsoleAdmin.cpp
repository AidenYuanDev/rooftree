/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Each command: its words checked on the main thread; the admin, the files it names and the posts or
 * accounts it names by name found on a worker; then the route's ApiOp with the body its page would
 * send, and its answer printed. Errors are the route's own: "Nothing was saved" lists the fields.
 */

#include "ConsoleAdmin.h"
#include "ApiCall.h"
#include "ApiJson.h"
#include "ApiOperations.h"
#include "BlogDatabase.h"
#include "BlogSettings.h"
#include "BlogWorld.h"
#include "Field.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"
#include "StringConvert.h"
#include "Util.h"
#include <boost/json/serialize.hpp>
#include <filesystem>
#include <fstream>

using Rooftree::Server::CommandHandler;
using Rooftree::Server::Session;
namespace http = boost::beast::http;

namespace Blog
{
namespace
{
constexpr std::size_t MaxConsoleFile = 256 * 1024 * 1024;

struct Lookup
{
    enum Kind { Post, Account } What;
    std::string Text;
};

// What a worker found for a command
struct Prepared
{
    AuthInfo Admin;
    std::vector<std::string> Files;     // the contents, in the order named
    std::vector<uint32> Ids;            // the lookups'
    std::vector<UserRow> Accounts;      // the Account lookups', in their order
    std::string Error;
};

struct CallSpec
{
    ApiOp Op;
    std::string Target;
    std::vector<std::pair<std::string, std::string>> Path = {};
    Optional<boost::json::object> Body = {};
    std::string RawBody = {};               // without Body: sent as it is, with ContentType
    std::string ContentType = {};
};

using Print = std::function<void(CommandHandler& handler, boost::json::value const& body, AuthInfo const& admin)>;
using Make = std::function<Optional<CallSpec>(CommandHandler& handler, Prepared& prepared)>;

std::string Str(boost::json::value const& value, std::string_view key)
{
    boost::json::object const* object = value.if_object();
    boost::json::value const* found = object ? object->if_contains(key) : nullptr;
    if (!found || found->is_null())
        return {};
    if (found->is_string())
        return std::string(found->as_string());
    // 2.3, not JSON's 2.3E0
    if (found->is_double())
        return Rooftree::StringFormat("{:.1f}", found->as_double());
    return boost::json::serialize(*found);
}

boost::json::value const& Get(boost::json::value const& value, std::string_view key)
{
    static boost::json::value const null;
    boost::json::object const* object = value.if_object();
    boost::json::value const* found = object ? object->if_contains(key) : nullptr;
    return found ? *found : null;
}

// "2026-10-28T10:00:00Z" as "2026-10-28 10:00 UTC", nothing for none
std::string ShortTime(std::string const& iso)
{
    if (iso.size() < 16)
        return iso;
    return iso.substr(0, 10) + " " + iso.substr(11, 5) + " UTC";
}

std::string Join(boost::json::value const& array, std::string_view separator = ", ")
{
    std::string out;
    if (boost::json::array const* items = array.if_array())
        for (boost::json::value const& item : *items)
            out += (out.empty() ? "" : std::string(separator)) + (item.is_string() ? std::string(item.as_string()) : boost::json::serialize(item));
    return out;
}

std::string Excerpt(std::string text, std::size_t max = 60)
{
    std::ranges::replace(text, '\n', ' ');
    if (Rooftree::Utf8Length(text) <= max)
        return text;
    Rooftree::Utf8Truncate(text, max);
    return text + "...";
}

std::size_t Characters(std::string const& text)
{
    return Rooftree::Utf8Length(text);
}

void PrintError(CommandHandler& handler, http::status status, boost::json::value const& body)
{
    boost::json::value const& error = Get(body, "error");
    std::string code = Str(error, "code");
    boost::json::object const* fields = Get(error, "fields").if_object();
    if (code == "not_found")
        return handler.SendSysMessage("Not found");
    if (fields && !fields->empty())
    {
        std::string out = "Nothing was saved:";
        for (auto const& [key, value] : *fields)
            out += Rooftree::StringFormat("\n  {}: {}", std::string_view(key), value.is_string() ? std::string(value.as_string()) : boost::json::serialize(value));
        return handler.SendSysMessage(out);
    }
    std::string message = Str(error, "message");
    handler.PSendSysMessage("Failed: {} ({}){}", code.empty() ? "error" : code, unsigned(status), message.empty() ? "" : ", " + message);
}

// Main thread: op as admin, the answer to print (or its error)
void Call(CommandHandler& handler, AuthInfo const& admin, CallSpec spec, Print print)
{
    // Only an operation of the table (ApiOperations.h): what the console does, the routes and packets do.
    // Its JSON becomes the operation's table at the same edge as a route's (ApiJson.h)
    ApiOperation const& operation = GetApiOperation(spec.Op);
    std::string body = spec.Body ? boost::json::serialize(*spec.Body) : std::move(spec.RawBody);
    JsonRequest json = RequestFromJson(operation, body, spec.Body ? std::string_view("application/json") : std::string_view(spec.ContentType),
        spec.Target, spec.Path);
    if (!json.Code.empty())
        return PrintError(handler, json.Status, boost::json::object{ { "error", { { "code", json.Code } } } });

    ApiRequest call = std::make_shared<ApiCall>(ApiCall::Params{
        .Auth = admin,
        .Opcode = operation.Opcode.Value,
        .Request = std::move(json.Request),
        .Errors = std::move(json.Errors),
        .ClientAddress = boost::asio::ip::address_v4::loopback(),
        .UserAgent = "blog console",
    }, [handler = handler.shared_from_this(), admin, print = std::move(print), &operation](http::status status, std::string payload,
        LoginChange const& /*login*/)
    {
        boost::json::value body = AnswerToJson(operation, status, payload);
        if (unsigned(status) >= 300)
            return PrintError(*handler, status, body);
        print(*handler, body, admin);
    });
    handler.GetSession().Spawn(operation.Op(handler.GetSession(), call));
}

// Worker
Optional<UserRow> WorkerFindUser(std::string const& account)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FOR_LOGIN);
    stmt->setString(0, account);
    stmt->setString(1, account);
    if (PreparedQueryResult result = BlogDatabase.Query(stmt))
        return UserRow::FromFields(result->Fetch());
    return {};
}

// The admin, the files and the lookups on a worker, then make and the op on the main thread
Rooftree::Task<void> PrepareAndCall(CommandHandler& handler, std::vector<std::string> files, std::vector<Lookup> lookups, Make make, Print print)
{
    Prepared prepared = co_await handler.GetSession().OnWorker([files = std::move(files), lookups = std::move(lookups)]
    {
        Prepared prepared;
        PreparedQueryResult admin = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_FIRST_ADMIN));
        if (!admin)
        {
            prepared.Error = "There is no admin yet: blog --create-admin";
            return prepared;
        }
        prepared.Admin.User = UserRow::FromFields(admin->Fetch());

        for (std::string const& path : files)
        {
            std::string& data = prepared.Files.emplace_back();
            if (!ReadConsoleFile(path, data, prepared.Error))
                return prepared;
        }

        for (Lookup const& lookup : lookups)
        {
            if (lookup.What == Lookup::Account)
            {
                Optional<UserRow> user = WorkerFindUser(lookup.Text);
                if (!user)
                {
                    prepared.Error = Rooftree::StringFormat("There is no account {}, 'lookup account' finds one by a part of it", lookup.Text);
                    return prepared;
                }
                prepared.Ids.push_back(user->Id);
                prepared.Accounts.push_back(std::move(*user));
                continue;
            }

            // A post: its number, or its slug
            if (Optional<uint32> id = Rooftree::StringTo<uint32>(lookup.Text))
            {
                prepared.Ids.push_back(*id);
                continue;
            }
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_ID_BY_SLUG);
            stmt->setString(0, lookup.Text);
            PreparedQueryResult post = BlogDatabase.Query(stmt);
            if (!post)
            {
                prepared.Error = Rooftree::StringFormat("There is no post {}, 'post list' shows them", lookup.Text);
                return prepared;
            }
            prepared.Ids.push_back(post->Fetch()[0].GetUInt32());
        }
        return prepared;
    });
    if (!prepared.Error.empty())
        co_return handler.SendSysMessage(prepared.Error);
    if (Optional<CallSpec> spec = make(handler, prepared))
        Call(handler, prepared.Admin, std::move(*spec), print);
}

void RunAsAdmin(CommandHandler& handler, std::vector<std::string> files, std::vector<Lookup> lookups, Make make, Print print)
{
    handler.Spawn(PrepareAndCall(handler, std::move(files), std::move(lookups), std::move(make), std::move(print)));
}

// The common case: nothing to read first
void RunAsAdmin(CommandHandler& handler, CallSpec spec, Print print)
{
    RunAsAdmin(handler, {}, {}, [spec = std::move(spec)](CommandHandler&, Prepared&) -> Optional<CallSpec> { return spec; }, std::move(print));
}

Print Saying(std::string text)
{
    return [text = std::move(text)](CommandHandler& handler, boost::json::value const&, AuthInfo const&) { handler.SendSysMessage(text); };
}

bool IsLang(std::string_view lang)
{
    return lang == "zh" || lang == "en";
}

Optional<bool> OnOff(std::string_view text)
{
    if (text == "on" || text == "yes" || text == "true")
        return true;
    if (text == "off" || text == "no" || text == "false")
        return false;
    return {};
}

// "2026-10-30 09:00+08:00", "2026-10-30T01:00Z", with or without seconds: the API's UTC form
Optional<std::string> ParseConsoleTime(std::string_view text)
{
    auto number = [&](std::size_t at, std::size_t length) -> Optional<int>
    {
        if (at + length > text.size())
            return {};
        return Rooftree::StringTo<int>(text.substr(at, length));
    };
    if (text.size() < 17 || text[4] != '-' || text[7] != '-' || (text[10] != ' ' && text[10] != 'T') || text[13] != ':')
        return {};
    Optional<int> year = number(0, 4), month = number(5, 2), day = number(8, 2), hour = number(11, 2), minute = number(14, 2);
    int second = 0;
    std::size_t zone = 16;
    if (text.size() > 16 && text[16] == ':')
    {
        Optional<int> seconds = number(17, 2);
        if (!seconds)
            return {};
        second = *seconds;
        zone = 19;
    }
    if (!year || !month || !day || !hour || !minute)
        return {};

    int offsetMinutes = 0;
    std::string_view tz = text.substr(zone);
    if (tz != "Z")
    {
        if (tz.size() != 6 || (tz[0] != '+' && tz[0] != '-') || tz[3] != ':')
            return {};
        Optional<int> h = Rooftree::StringTo<int>(tz.substr(1, 2)), m = Rooftree::StringTo<int>(tz.substr(4, 2));
        if (!h || !m)
            return {};
        offsetMinutes = (tz[0] == '-' ? -1 : 1) * (*h * 60 + *m);
    }

    std::chrono::year_month_day date{ std::chrono::year(*year), std::chrono::month(unsigned(*month)), std::chrono::day(unsigned(*day)) };
    if (!date.ok() || *hour > 23 || *minute > 59 || second > 59)
        return {};
    std::chrono::sys_seconds time = std::chrono::sys_days(date) + std::chrono::hours(*hour) + std::chrono::minutes(*minute)
        + std::chrono::seconds(second) - std::chrono::minutes(offsetMinutes);
    return FormatTime(SystemTimePoint(time));
}

// "5G", "500M", "1024" (bytes); nothing when not a size
Optional<uint64> ParseSize(std::string_view text)
{
    uint64 unit = 1;
    if (!text.empty())
    {
        switch (std::toupper(static_cast<unsigned char>(text.back())))
        {
            case 'K': unit = 1024; break;
            case 'M': unit = 1024 * 1024; break;
            case 'G': unit = 1024ull * 1024 * 1024; break;
            case 'T': unit = 1024ull * 1024 * 1024 * 1024; break;
            default: break;
        }
        if (unit != 1)
            text.remove_suffix(1);
    }
    Optional<uint64> number = Rooftree::StringTo<uint64>(text);
    if (!number)
        return {};
    return *number * unit;
}

std::string FormatSize(uint64 bytes)
{
    if (bytes >= 1024ull * 1024 * 1024)
        return Rooftree::StringFormat("{:.1f} GB", double(bytes) / (1024.0 * 1024 * 1024));
    if (bytes >= 1024 * 1024)
        return Rooftree::StringFormat("{:.1f} MB", double(bytes) / (1024.0 * 1024));
    return Rooftree::StringFormat("{} KB", (bytes + 1023) / 1024);
}

std::vector<std::string> Words(std::string_view args)
{
    return Rooftree::Server::SplitCommandArgs(args);
}

// The words from first on, as one text
std::string Rest(std::vector<std::string> const& words, std::size_t first)
{
    std::string out;
    for (std::size_t i = first; i < words.size(); ++i)
        out += (i == first ? "" : " ") + words[i];
    return out;
}

// ---- posts

std::string PostLine(boost::json::value const& post)
{
    std::string when = Str(post, "publishAt").empty() ? ShortTime(Str(post, "publishedAt")) : "at " + ShortTime(Str(post, "publishAt"));
    bool review = Get(post, "reviewRequested").is_bool() && Get(post, "reviewRequested").as_bool();
    return Rooftree::StringFormat("{:>4}  {:<9}  {:<32}  {:<5}  {}  {}{}", Str(post, "id"), Str(post, "status"), Str(post, "slug"),
        Join(Get(post, "translations"), ","), Excerpt(Str(post, "title"), 40), when, review ? "  (asks to be published)" : "");
}

void PrintPost(CommandHandler& handler, boost::json::value const& post, std::string_view verb)
{
    std::string out = Rooftree::StringFormat("{}post {} ({}): {}", verb, Str(post, "slug"), Str(post, "id"), Str(post, "status"));
    if (!Str(post, "publishAt").empty())
        out += ", publishes at " + ShortTime(Str(post, "publishAt"));
    else if (!Str(post, "publishedAt").empty())
        out += ", published " + ShortTime(Str(post, "publishedAt"));
    if (boost::json::object const* translations = Get(post, "translations").if_object())
        for (auto const& [lang, text] : *translations)
            out += Rooftree::StringFormat("\n  {}{}: {}{}, {} characters", std::string_view(lang), lang == Str(post, "originalLang") ? " (original)" : "",
                Str(text, "title"), Str(text, "summary").empty() ? "" : " - " + Excerpt(Str(text, "summary"), 40), Characters(Str(text, "body")));
    std::string tags = Join(Get(post, "tags"));
    out += Rooftree::StringFormat("\n  tags: {}, comments {}{}", tags.empty() ? "none" : tags, Get(post, "commentsEnabled").is_bool()
        && !Get(post, "commentsEnabled").as_bool() ? "off" : "on", Str(post, "cover").empty() ? "" : ", cover " + Str(post, "cover"));
    handler.SendSysMessage(out);
}

Print PrintingPost(std::string verb)
{
    return [verb = std::move(verb)](CommandHandler& handler, boost::json::value const& post, AuthInfo const&) { PrintPost(handler, post, verb); };
}

bool HandlePostList(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    std::string status;
    uint32 page = 1;
    for (std::string const& word : words)
    {
        if (word == "draft" || word == "published" || word == "review")
            status = word;
        else if (Optional<uint32> number = Rooftree::StringTo<uint32>(word); number && *number)
            page = *number;
        else
            return false;
    }

    RunAsAdmin(handler, { .Op = &AdminListPosts, .Target = Rooftree::StringFormat("/api/admin/posts?status={}&page={}", status, page) },
        [status, page](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        boost::json::array const* posts = Get(body, "posts").if_array();
        if (!posts || posts->empty())
            return handler.SendSysMessage(page > 1 ? "No more posts" : "No posts");
        std::string out = "  id  status     slug                              langs  title  published";
        for (boost::json::value const& post : *posts)
            out += "\n" + PostLine(post);
        if (Get(body, "hasMore").is_bool() && Get(body, "hasMore").as_bool())
            out += Rooftree::StringFormat("\nMore: post list {}{}", status.empty() ? "" : status + " ", page + 1);
        handler.SendSysMessage(out);
    });
    return true;
}

// The post named by the first word, then make with its id
bool WithPost(CommandHandler& handler, std::string const& post, std::vector<std::string> files,
    std::function<Optional<CallSpec>(CommandHandler&, uint32 id, Prepared&)> make, Print print)
{
    RunAsAdmin(handler, std::move(files), { { Lookup::Post, post } }, [make = std::move(make)](CommandHandler& handler, Prepared& prepared)
    {
        return make(handler, prepared.Ids[0], prepared);
    }, std::move(print));
    return true;
}

CallSpec PostCall(ApiOp op, uint32 id, Optional<boost::json::object> body = {})
{
    return { .Op = op, .Target = Rooftree::StringFormat("/api/admin/posts/{}", id), .Path = { { "id", std::to_string(id) } }, .Body = std::move(body) };
}

// A version's line: its id, language, kind, when, by whom, title, length, note
std::string RevisionLine(boost::json::value const& revision)
{
    std::string author = Str(Get(revision, "author"), "username");
    return Rooftree::StringFormat("{:>5}  {}  {:<9}  {}  {:<12}  {}  ({} characters){}", Str(revision, "id"), Str(revision, "lang"), Str(revision, "kind"),
        ShortTime(Str(revision, "createdAt")), author.empty() ? "(scheduled)" : author, Excerpt(Str(revision, "title"), 40), Str(revision, "length"),
        Str(revision, "note").empty() ? "" : ": " + Str(revision, "note"));
}

bool HandlePostRevisions(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<uint32> shown = words.size() == 2 ? Rooftree::StringTo<uint32>(words[1]) : Optional<uint32>();
    if (words.empty() || words.size() > 2 || (words.size() == 2 && !shown))
        return false;
    if (shown)
        return WithPost(handler, words[0], {}, [shown](CommandHandler&, uint32 id, Prepared&)
        {
            CallSpec call = PostCall(&AdminPostRevision, id);
            call.Target += Rooftree::StringFormat("/revisions/{}", *shown);
            call.Path.emplace_back("revision", std::to_string(*shown));
            return call;
        }, [](CommandHandler& handler, boost::json::value const& revision, AuthInfo const&)
        {
            std::string summary = Str(revision, "summary");
            handler.SendSysMessage(Rooftree::StringFormat("{}{}\n\n{}", RevisionLine(revision), summary.empty() ? "" : "\n  " + summary,
                Str(revision, "body")));
        });
    return WithPost(handler, words[0], {}, [](CommandHandler&, uint32 id, Prepared&)
    {
        CallSpec call = PostCall(&AdminPostRevisions, id);
        call.Target += "/revisions";
        return call;
    }, [](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        boost::json::array const* revisions = Get(body, "revisions").if_array();
        if (!revisions || revisions->empty())
            return handler.SendSysMessage("No versions");
        std::string out;
        for (boost::json::value const& revision : *revisions)
            out += (out.empty() ? "" : "\n") + RevisionLine(revision);
        handler.SendSysMessage(out);
    });
}

// post save-version <post> [note]: its working copy as a version, of each language it changed
bool HandlePostSaveVersion(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.empty())
        return false;
    std::string note = Rest(words, 1);
    return WithPost(handler, words[0], {}, [note](CommandHandler&, uint32 id, Prepared&)
    {
        CallSpec call = PostCall(&AdminSavePostVersion, id, boost::json::object{ { "note", note } });
        call.Target += "/revisions";
        return call;
    }, [](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        boost::json::array const* revisions = Get(body, "revisions").if_array();
        if (!revisions || revisions->empty())
            return handler.SendSysMessage("Nothing changed since its last version");
        std::string out = "Saved:";
        for (boost::json::value const& revision : *revisions)
            out += "\n" + RevisionLine(revision);
        handler.SendSysMessage(out);
    });
}

bool HandlePostRestore(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<uint32> revision = words.size() == 2 ? Rooftree::StringTo<uint32>(words[1]) : Optional<uint32>();
    if (!revision)
        return false;
    return WithPost(handler, words[0], {}, [revision](CommandHandler&, uint32 id, Prepared&)
    {
        CallSpec call = PostCall(&AdminRestorePostRevision, id, boost::json::object{});
        call.Target += Rooftree::StringFormat("/revisions/{}/restore", *revision);
        call.Path.emplace_back("revision", std::to_string(*revision));
        return call;
    }, Saying(Rooftree::StringFormat("Version {} is back in the working copy of {}; post publish {} gives it to readers", *revision, words[0], words[0])));
}

// Its owner and editors, with their rights
void PrintEditors(CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
{
    boost::json::value const& owner = Get(body, "owner");
    std::string out = Rooftree::StringFormat("owner: {} ({})", Str(owner, "username"), Str(owner, "displayName"));
    boost::json::array const* editors = Get(body, "editors").if_array();
    if (!editors || editors->empty())
        out += "\nNo editors";
    else
        for (boost::json::value const& editor : *editors)
        {
            std::string rights = Join(Get(editor, "rights"));
            out += Rooftree::StringFormat("\n  {} ({}), since {}: its text{}", Str(Get(editor, "user"), "username"), Str(Get(editor, "user"), "displayName"),
                ShortTime(Str(editor, "createdAt")), rights.empty() ? "" : ", " + rights);
        }
    handler.SendSysMessage(out);
}

bool HandlePostEditors(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 1)
        return false;
    return WithPost(handler, words[0], {}, [](CommandHandler&, uint32 id, Prepared&)
    {
        CallSpec call = PostCall(&AdminPostEditors, id);
        call.Target += "/editors";
        return call;
    }, &PrintEditors);
}

// post editor add <post> <account> [publish,settings,editors|none]: invites it, or sets its rights
bool HandlePostEditorAdd(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() < 2 || words.size() > 3)
        return false;
    boost::json::array rights;
    if (words.size() == 3 && words[2] != "none")
        for (std::string const& right : SplitList(words[2]))
            rights.emplace_back(right);
    RunAsAdmin(handler, {}, { { Lookup::Post, words[0] }, { Lookup::Account, words[1] } }, [rights](CommandHandler&, Prepared& prepared)
    {
        CallSpec call = PostCall(&AdminSetPostEditor, prepared.Ids[0], boost::json::object{ { "username", prepared.Accounts[0].Username }, { "rights", rights } });
        call.Target += "/editors";
        return Optional<CallSpec>(std::move(call));
    }, &PrintEditors);
    return true;
}

bool HandlePostEditorRemove(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 2)
        return false;
    RunAsAdmin(handler, {}, { { Lookup::Post, words[0] }, { Lookup::Account, words[1] } }, [](CommandHandler&, Prepared& prepared)
    {
        uint32 user = prepared.Accounts[0].Id;
        CallSpec call = PostCall(&AdminRemovePostEditor, prepared.Ids[0]);
        call.Target += Rooftree::StringFormat("/editors/{}", user);
        call.Path.emplace_back("user", std::to_string(user));
        return Optional<CallSpec>(std::move(call));
    }, &PrintEditors);
    return true;
}

// The post to another account; the one before edits it, with every right
bool HandlePostOwner(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 2)
        return false;
    RunAsAdmin(handler, {}, { { Lookup::Post, words[0] }, { Lookup::Account, words[1] } }, [](CommandHandler&, Prepared& prepared)
    {
        return Optional<CallSpec>(PostCall(&AdminUpdatePost, prepared.Ids[0], boost::json::object{ { "owner", prepared.Accounts[0].Username } }));
    }, [](CommandHandler& handler, boost::json::value const& post, AuthInfo const&)
    {
        handler.SendSysMessage(Rooftree::StringFormat("{} is now {}'s", Str(post, "slug"), Str(Get(post, "owner"), "username")));
    });
    return true;
}

bool HandlePostShow(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 1)
        return false;
    return WithPost(handler, words[0], {}, [](CommandHandler&, uint32 id, Prepared&) { return PostCall(&AdminGetPost, id); }, PrintingPost(""));
}

bool HandlePostCreate(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() < 4 || words.size() > 5 || !IsLang(words[1]) || (words.size() == 5 && words[4] != "published" && words[4] != "draft"))
        return false;

    std::string slug = words[0], lang = words[1], title = words[2], status = words.size() == 5 ? words[4] : "draft";
    RunAsAdmin(handler, { words[3] }, {}, [=](CommandHandler&, Prepared& prepared) -> Optional<CallSpec>
    {
        boost::json::object body{ { "slug", slug }, { "originalLang", lang }, { "status", status },
            { "translations", { { lang, { { "title", title }, { "body", prepared.Files[0] } } } } } };
        return CallSpec{ .Op = &AdminCreatePost, .Target = "/api/admin/posts", .Body = std::move(body) };
    }, PrintingPost("Created "));
    return true;
}

bool HandlePostEdit(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() < 3)
        return false;

    std::string const& field = words[1];
    std::string value = Rest(words, 2);
    boost::json::object body;
    if (field == "slug" || field == "status" || field == "cover")
        body[field] = field == "cover" && value == "none" ? std::string() : value;
    else if (field == "lang")
        body["originalLang"] = value;
    else if (field == "comments")
    {
        Optional<bool> on = OnOff(value);
        if (!on)
            return false;
        body["commentsEnabled"] = *on;
    }
    else if (field == "tags")
    {
        boost::json::array tags;
        if (value != "none")
            for (std::string_view tag : Rooftree::Tokenize(value, ',', false))
                tags.emplace_back(tag);
        body["tags"] = std::move(tags);
    }
    else if (field == "publish-at")
    {
        if (value == "none")
            body["publishAt"] = nullptr;
        else if (Optional<std::string> at = ParseConsoleTime(value))
            body["publishAt"] = *at;
        else
        {
            handler.SendSysMessage("A time with its zone: 2026-10-30 09:00+08:00 or 2026-10-30T01:00Z (none: no time)");
            return true;
        }
    }
    else
        return false;

    return WithPost(handler, words[0], {}, [body](CommandHandler&, uint32 id, Prepared&) { return PostCall(&AdminUpdatePost, id, body); }, PrintingPost("Saved "));
}

bool SetPostStatus(CommandHandler& handler, std::string_view args, std::string status)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 1)
        return false;
    return WithPost(handler, words[0], {}, [status](CommandHandler&, uint32 id, Prepared&)
    {
        return PostCall(&AdminUpdatePost, id, boost::json::object{ { "status", status } });
    }, PrintingPost(status == "published" ? "Published " : "Unpublished "));
}

// With what its editors wrote (its working copy), a draft or a published post alike
bool HandlePostPublish(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 1)
        return false;
    return WithPost(handler, words[0], {}, [](CommandHandler&, uint32 id, Prepared&)
    {
        CallSpec call = PostCall(&AdminPublishPost, id);
        call.Target += "/publish";
        return call;
    }, PrintingPost("Published "));
}
bool HandlePostUnpublish(CommandHandler& handler, std::string_view args) { return SetPostStatus(handler, args, "draft"); }

// A writer's asking to publish it, declined (publishing it is post publish)
bool HandlePostDecline(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 1)
        return false;
    return WithPost(handler, words[0], {}, [](CommandHandler&, uint32 id, Prepared&)
    {
        return PostCall(&AdminUpdatePost, id, boost::json::object{ { "reviewRequested", false } });
    }, Saying("Declined: " + words[0] + " is no longer asked to be published"));
}

bool HandlePostDelete(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 1)
        return false;
    return WithPost(handler, words[0], {}, [](CommandHandler&, uint32 id, Prepared&) { return PostCall(&AdminDeletePost, id); },
        Saying("Deleted " + words[0]));
}

bool HandlePostTranslationSet(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() < 4 || !IsLang(words[1]))
        return false;
    std::string lang = words[1], title = words[2], summary = Rest(words, 4);
    return WithPost(handler, words[0], { words[3] }, [=](CommandHandler&, uint32 id, Prepared& prepared)
    {
        boost::json::object text{ { "title", title }, { "body", prepared.Files[0] } };
        if (!summary.empty())
            text["summary"] = summary;
        return PostCall(&AdminUpdatePost, id, boost::json::object{ { "translations", { { lang, std::move(text) } } } });
    }, PrintingPost("Saved "));
}

bool HandlePostTranslationDelete(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 2 || !IsLang(words[1]))
        return false;
    std::string lang = words[1];
    return WithPost(handler, words[0], {}, [lang](CommandHandler&, uint32 id, Prepared&)
    {
        return PostCall(&AdminUpdatePost, id, boost::json::object{ { "translations", { { lang, nullptr } } } });
    }, PrintingPost("Saved "));
}

// ---- tags

bool HandleTagList(CommandHandler& handler, std::string_view /*args*/)
{
    std::string out;
    for (auto const& [slug, tag] : sWorld->GetTags())
        out += Rooftree::StringFormat("{}{:<24} {:<16} {:<20} {} posts", out.empty() ? "" : "\n", tag.Slug, tag.NameZh, tag.NameEn, tag.GetPostCount({}));
    handler.SendSysMessage(out.empty() ? "No tags" : out);
    return true;
}

bool HandleTagAdd(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 3)
        return false;
    RunAsAdmin(handler, { .Op = &AdminCreateTag, .Target = "/api/admin/tags",
        .Body = boost::json::object{ { "slug", words[0] }, { "nameZh", words[1] }, { "nameEn", words[2] } } }, Saying("Added tag " + words[0]));
    return true;
}

bool HandleTagEdit(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 3)
        return false;
    RunAsAdmin(handler, { .Op = &AdminUpdateTag, .Target = "/api/admin/tags/" + words[0], .Path = { { "slug", words[0] } },
        .Body = boost::json::object{ { "nameZh", words[1] }, { "nameEn", words[2] } } }, Saying("Saved tag " + words[0]));
    return true;
}

bool HandleTagDelete(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 1)
        return false;
    RunAsAdmin(handler, { .Op = &AdminDeleteTag, .Target = "/api/admin/tags/" + words[0], .Path = { { "slug", words[0] } } },
        Saying("Deleted tag " + words[0]));
    return true;
}

// ---- notes and thinking, by id

Optional<uint32> IdWord(std::vector<std::string> const& words, std::size_t index = 0)
{
    if (index >= words.size())
        return {};
    Optional<uint32> id = Rooftree::StringTo<uint32>(words[index]);
    return id && *id ? id : Optional<uint32>();
}

CallSpec ItemCall(std::string_view kind, ApiOp op, uint32 id, Optional<boost::json::object> body = {})
{
    return { .Op = op, .Target = Rooftree::StringFormat("/api/admin/{}/{}", kind, id), .Path = { { "id", std::to_string(id) } }, .Body = std::move(body) };
}

void PrintNote(CommandHandler& handler, boost::json::value const& note, std::string_view verb)
{
    std::string out = Rooftree::StringFormat("{}note {}: {}", verb, Str(note, "id"), Str(note, "status"));
    if (!Str(note, "publishedAt").empty())
        out += ", published " + ShortTime(Str(note, "publishedAt"));
    if (boost::json::object const* translations = Get(note, "translations").if_object())
        for (auto const& [lang, text] : *translations)
            out += Rooftree::StringFormat("\n  {}{}: {}, {} characters", std::string_view(lang), lang == Str(note, "originalLang") ? " (original)" : "",
                Str(text, "title"), Characters(Str(text, "body")));
    std::string mood = Str(note, "mood"), weather = Str(note, "weather");
    if (!mood.empty() || !weather.empty())
        out += Rooftree::StringFormat("\n  mood: {}, weather: {}", mood.empty() ? "-" : mood, weather.empty() ? "-" : weather);
    handler.SendSysMessage(out);
}

Print PrintingNote(std::string verb)
{
    return [verb = std::move(verb)](CommandHandler& handler, boost::json::value const& note, AuthInfo const&) { PrintNote(handler, note, verb); };
}

bool HandleNoteList(CommandHandler& handler, std::string_view args)
{
    std::string status;
    uint32 page = 1;
    for (std::string const& word : Words(args))
    {
        if (word == "draft" || word == "published")
            status = word;
        else if (Optional<uint32> number = Rooftree::StringTo<uint32>(word); number && *number)
            page = *number;
        else
            return false;
    }

    RunAsAdmin(handler, { .Op = &AdminListNotes, .Target = Rooftree::StringFormat("/api/admin/notes?status={}&page={}", status, page) },
        [status, page](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        boost::json::array const* notes = Get(body, "notes").if_array();
        if (!notes || notes->empty())
            return handler.SendSysMessage(page > 1 ? "No more notes" : "No notes");
        std::string out = "  id  status     langs  title";
        for (boost::json::value const& note : *notes)
            out += Rooftree::StringFormat("\n{:>4}  {:<9}  {:<5}  {}  {}", Str(note, "id"), Str(note, "status"), Join(Get(note, "translations"), ","),
                Excerpt(Str(note, "title"), 40), ShortTime(Str(note, "publishedAt")));
        if (Get(body, "hasMore").is_bool() && Get(body, "hasMore").as_bool())
            out += Rooftree::StringFormat("\nMore: note list {}{}", status.empty() ? "" : status + " ", page + 1);
        handler.SendSysMessage(out);
    });
    return true;
}

bool HandleNoteShow(CommandHandler& handler, std::string_view args)
{
    Optional<uint32> id = IdWord(Words(args));
    if (!id)
        return false;
    RunAsAdmin(handler, ItemCall("notes", &AdminGetNote, *id), PrintingNote(""));
    return true;
}

bool HandleNoteCreate(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() < 3 || words.size() > 4 || !IsLang(words[0]) || (words.size() == 4 && words[3] != "published" && words[3] != "draft"))
        return false;
    std::string lang = words[0], title = words[1], status = words.size() == 4 ? words[3] : "draft";
    RunAsAdmin(handler, { words[2] }, {}, [=](CommandHandler&, Prepared& prepared) -> Optional<CallSpec>
    {
        boost::json::object body{ { "originalLang", lang }, { "status", status },
            { "translations", { { lang, { { "title", title }, { "body", prepared.Files[0] } } } } } };
        return CallSpec{ .Op = &AdminCreateNote, .Target = "/api/admin/notes", .Body = std::move(body) };
    }, PrintingNote("Created "));
    return true;
}

bool HandleNoteEdit(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<uint32> id = IdWord(words);
    if (!id || words.size() < 3)
        return false;
    std::string const& field = words[1];
    std::string value = Rest(words, 2);
    boost::json::object body;
    if (field == "status" || field == "mood" || field == "weather" || field == "cover")
        body[field] = value == "none" && field != "status" ? std::string() : value;
    else if (field == "lang")
        body["originalLang"] = value;
    else if (field == "comments")
    {
        Optional<bool> on = OnOff(value);
        if (!on)
            return false;
        body["commentsEnabled"] = *on;
    }
    else
        return false;
    RunAsAdmin(handler, ItemCall("notes", &AdminUpdateNote, *id, std::move(body)), PrintingNote("Saved "));
    return true;
}

bool SetNoteStatus(CommandHandler& handler, std::string_view args, std::string status)
{
    Optional<uint32> id = IdWord(Words(args));
    if (!id)
        return false;
    RunAsAdmin(handler, ItemCall("notes", &AdminUpdateNote, *id, boost::json::object{ { "status", status } }),
        PrintingNote(status == "published" ? "Published " : "Unpublished "));
    return true;
}

bool HandleNotePublish(CommandHandler& handler, std::string_view args) { return SetNoteStatus(handler, args, "published"); }
bool HandleNoteUnpublish(CommandHandler& handler, std::string_view args) { return SetNoteStatus(handler, args, "draft"); }

bool HandleNoteDelete(CommandHandler& handler, std::string_view args)
{
    Optional<uint32> id = IdWord(Words(args));
    if (!id)
        return false;
    RunAsAdmin(handler, ItemCall("notes", &AdminDeleteNote, *id), Saying(Rooftree::StringFormat("Deleted note {}", *id)));
    return true;
}

bool HandleNoteTranslationSet(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<uint32> id = IdWord(words);
    if (!id || words.size() != 4 || !IsLang(words[1]))
        return false;
    std::string lang = words[1], title = words[2];
    RunAsAdmin(handler, { words[3] }, {}, [=, id = *id](CommandHandler&, Prepared& prepared) -> Optional<CallSpec>
    {
        return ItemCall("notes", &AdminUpdateNote, id, boost::json::object{ { "translations", { { lang, { { "title", title }, { "body", prepared.Files[0] } } } } } });
    }, PrintingNote("Saved "));
    return true;
}

bool HandleNoteTranslationDelete(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<uint32> id = IdWord(words);
    if (!id || words.size() != 2 || !IsLang(words[1]))
        return false;
    RunAsAdmin(handler, ItemCall("notes", &AdminUpdateNote, *id, boost::json::object{ { "translations", { { words[1], nullptr } } } }), PrintingNote("Saved "));
    return true;
}

Print PrintingThinking(std::string verb)
{
    return [verb = std::move(verb)](CommandHandler& handler, boost::json::value const& item, AuthInfo const&)
    {
        std::string out = Rooftree::StringFormat("{}thinking {}: {}", verb, Str(item, "id"), Str(item, "status"));
        if (boost::json::object const* translations = Get(item, "translations").if_object())
            for (auto const& [lang, text] : *translations)
                out += Rooftree::StringFormat("\n  {}: {}", std::string_view(lang), Excerpt(Str(text, "body"), 80));
        handler.SendSysMessage(out);
    };
}

bool HandleThinkingList(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    uint32 page = words.empty() ? 1 : Rooftree::StringTo<uint32>(words[0]).value_or(0);
    if (!page || words.size() > 1)
        return false;
    RunAsAdmin(handler, { .Op = &AdminListThinking, .Target = Rooftree::StringFormat("/api/admin/thinking?page={}", page) },
        [page](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        boost::json::array const* items = Get(body, "thinking").if_array();
        if (!items)
            items = Get(body, "items").if_array();
        if (!items || items->empty())
            return handler.SendSysMessage(page > 1 ? "No more thinking" : "No thinking");
        std::string out;
        for (boost::json::value const& item : *items)
            out += Rooftree::StringFormat("{}{:>4}  {:<9}  {:<5}  {}", out.empty() ? "" : "\n", Str(item, "id"), Str(item, "status"),
                Join(Get(item, "translations"), ","), Excerpt(Str(item, "body"), 50));
        if (Get(body, "hasMore").is_bool() && Get(body, "hasMore").as_bool())
            out += Rooftree::StringFormat("\nMore: thinking list {}", page + 1);
        handler.SendSysMessage(out);
    });
    return true;
}

bool HandleThinkingAdd(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() < 2 || !IsLang(words[0]))
        return false;
    RunAsAdmin(handler, { .Op = &AdminCreateThinking, .Target = "/api/admin/thinking", .Body = boost::json::object{ { "originalLang", words[0] },
        { "status", "published" }, { "translations", { { words[0], { { "body", Rest(words, 1) } } } } } } }, PrintingThinking("Added "));
    return true;
}

bool HandleThinkingEdit(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<uint32> id = IdWord(words);
    if (!id || words.size() < 3 || !IsLang(words[1]))
        return false;
    RunAsAdmin(handler, ItemCall("thinking", &AdminUpdateThinking, *id,
        boost::json::object{ { "translations", { { words[1], { { "body", Rest(words, 2) } } } } } }), PrintingThinking("Saved "));
    return true;
}

bool SetThinkingStatus(CommandHandler& handler, std::string_view args, std::string status)
{
    Optional<uint32> id = IdWord(Words(args));
    if (!id)
        return false;
    RunAsAdmin(handler, ItemCall("thinking", &AdminUpdateThinking, *id, boost::json::object{ { "status", status } }),
        PrintingThinking(status == "published" ? "Published " : "Unpublished "));
    return true;
}

bool HandleThinkingPublish(CommandHandler& handler, std::string_view args) { return SetThinkingStatus(handler, args, "published"); }
bool HandleThinkingUnpublish(CommandHandler& handler, std::string_view args) { return SetThinkingStatus(handler, args, "draft"); }

bool HandleThinkingDelete(CommandHandler& handler, std::string_view args)
{
    Optional<uint32> id = IdWord(Words(args));
    if (!id)
        return false;
    RunAsAdmin(handler, ItemCall("thinking", &AdminDeleteThinking, *id), Saying(Rooftree::StringFormat("Deleted thinking {}", *id)));
    return true;
}

// ---- comments

bool HandleCommentList(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    std::string status;
    std::string cursor;
    std::string passage;
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        if (words[i] == "visible" || words[i] == "hidden" || words[i] == "deleted")
            status = words[i];
        else if (words[i] == "orphaned")
            passage = words[i];
        else if (words[i] == "before" && i + 1 < words.size() && Rooftree::StringTo<uint32>(words[i + 1]))
            cursor = words[++i];
        else
            return false;
    }

    RunAsAdmin(handler, { .Op = &AdminListComments, .Target = Rooftree::StringFormat("/api/admin/comments?status={}&passage={}&cursor={}", status, passage, cursor) },
        [status = passage.empty() ? status : (status.empty() ? passage : status + " " + passage)](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        boost::json::array const* comments = Get(body, "comments").if_array();
        if (!comments || comments->empty())
            return handler.SendSysMessage("No comments");
        std::string out;
        for (boost::json::value const& comment : *comments)
        {
            boost::json::value const& author = Get(comment, "author");
            std::string where = Str(comment, "postSlug").empty() ? Str(comment, "target") : Str(comment, "postSlug");
            out += Rooftree::StringFormat("{}{:>5}  {:<7}  {} {}  {:<16} {:<24} {}", out.empty() ? "" : "\n", Str(comment, "id"), Str(comment, "status"),
                ShortTime(Str(comment, "createdAt")), Str(comment, "ip"), Str(author, "username") + (Get(author, "banned").is_bool()
                && Get(author, "banned").as_bool() ? " (banned)" : ""), Excerpt(where, 24), Excerpt(Str(comment, "body"), 50));
            // On a passage: what was selected, and whether the post still has it
            if (boost::json::value const& passage = Get(comment, "passage"); passage.is_object())
                out += Rooftree::StringFormat("\n       on \"{}\" ({}, {})", Excerpt(Str(passage, "quote"), 60), Str(passage, "lang"), Str(passage, "status"));
        }
        if (!Str(body, "nextCursor").empty())
            out += Rooftree::StringFormat("\nMore: comment list {}before {}", status.empty() ? "" : status + " ", Str(body, "nextCursor"));
        handler.SendSysMessage(out);
    });
    return true;
}

bool SetCommentStatus(CommandHandler& handler, std::string_view args, std::string status)
{
    Optional<uint32> id = IdWord(Words(args));
    if (!id)
        return false;
    RunAsAdmin(handler, ItemCall("comments", &AdminModerateComment, *id, boost::json::object{ { "status", status } }),
        Saying(Rooftree::StringFormat("Comment {} is {} now", *id, status)));
    return true;
}

bool HandleCommentKeep(CommandHandler& handler, std::string_view args)
{
    Optional<uint32> id = IdWord(Words(args));
    if (!id)
        return false;
    RunAsAdmin(handler, ItemCall("comments", &AdminModerateComment, *id, boost::json::object{ { "passage", "kept" } }),
        Saying(Rooftree::StringFormat("Comment {} keeps its passage under \"the text changed\"", *id)));
    return true;
}

bool HandleCommentHide(CommandHandler& handler, std::string_view args) { return SetCommentStatus(handler, args, "hidden"); }
bool HandleCommentShow(CommandHandler& handler, std::string_view args) { return SetCommentStatus(handler, args, "visible"); }

// ---- chat: the square and the groups (never the direct chats)

// "square", or a group by its id ("7" or "group:7")
Optional<std::string> ChatChannelWord(std::string const& word)
{
    if (word == "square")
        return word;
    Optional<uint32> id = Rooftree::StringTo<uint32>(word.starts_with("group:") ? std::string_view(word).substr(6) : std::string_view(word));
    return id && *id ? "group:" + std::to_string(*id) : Optional<std::string>();
}

Print SayingRemoved(std::string what)
{
    return [what = std::move(what)](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        handler.PSendSysMessage("{}: {} messages removed", what, Str(body, "removed"));
    };
}

bool HandleChatGroups(CommandHandler& handler, std::string_view /*args*/)
{
    RunAsAdmin(handler, { .Op = &AdminListChatGroups, .Target = "/api/admin/chat/groups" }, [](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        boost::json::array const* groups = Get(body, "groups").if_array();
        if (!groups || groups->empty())
            return handler.SendSysMessage("No groups");
        std::string out;
        for (boost::json::value const& group : *groups)
            out += Rooftree::StringFormat("{}{:>5}  {:<24} owner {:<16} {:>3} members  {:>5} messages  since {}", out.empty() ? "" : "\n", Str(group, "id"),
                Excerpt(Str(group, "name"), 24), Str(group, "ownerUsername"), Str(group, "members"), Str(group, "messages"), ShortTime(Str(group, "createdAt")));
        handler.SendSysMessage(out);
    });
    return true;
}

bool HandleChatMessages(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<std::string> channel = words.empty() ? Optional<std::string>() : ChatChannelWord(words[0]);
    std::string cursor;
    if (!channel || (words.size() != 1 && (words.size() != 3 || words[1] != "before" || !Rooftree::StringTo<uint64>(words[2]))))
        return false;
    if (words.size() == 3)
        cursor = words[2];

    RunAsAdmin(handler, { .Op = &AdminListChatMessages, .Target = Rooftree::StringFormat("/api/admin/chat/messages?channel={}&cursor={}", *channel, cursor) },
        [channel = *channel](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        boost::json::array const* messages = Get(body, "messages").if_array();
        if (!messages || messages->empty())
            return handler.SendSysMessage("No messages");
        std::string out;
        for (boost::json::value const& message : *messages)
            out += Rooftree::StringFormat("{}{:>7}  {}  {:<16} {}", out.empty() ? "" : "\n", Str(message, "id"), ShortTime(Str(message, "createdAt")),
                Str(message, "senderUsername"), Excerpt(Str(message, "content"), 70));
        if (!Str(body, "nextCursor").empty())
            out += Rooftree::StringFormat("\nMore: chat messages {} before {}", channel, Str(body, "nextCursor"));
        handler.SendSysMessage(out);
    });
    return true;
}

bool HandleChatDelete(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<uint64> id = words.size() == 1 ? Rooftree::StringTo<uint64>(words[0]) : Optional<uint64>();
    if (!id || !*id)
        return false;
    RunAsAdmin(handler, { .Op = &AdminDeleteChatMessage, .Target = Rooftree::StringFormat("/api/admin/chat/messages/{}", *id),
        .Path = { { "id", std::to_string(*id) } } }, Saying(Rooftree::StringFormat("Chat message {} deleted", *id)));
    return true;
}

bool HandleChatClear(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<std::string> channel = words.size() == 1 ? ChatChannelWord(words[0]) : Optional<std::string>();
    if (!channel)
        return false;
    RunAsAdmin(handler, { .Op = &AdminClearChatChannel, .Target = Rooftree::StringFormat("/api/admin/chat/channels/{}/messages", *channel),
        .Path = { { "channel", *channel } } }, SayingRemoved("Cleared " + *channel));
    return true;
}

bool HandleChatDissolve(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<std::string> channel = words.size() == 1 ? ChatChannelWord(words[0]) : Optional<std::string>();
    if (!channel || *channel == "square")
        return false;
    std::string id = channel->substr(6);
    RunAsAdmin(handler, { .Op = &AdminDeleteChatGroup, .Target = "/api/admin/chat/groups/" + id, .Path = { { "id", id } } },
        SayingRemoved("Dissolved group " + id));
    return true;
}

// ---- friends' links and projects

std::string FriendLine(boost::json::value const& link)
{
    std::string who = Str(Get(link, "applicant"), "username");
    return Rooftree::StringFormat("{:>4}  {:<8}  {:<20} {:<36} {}{}", Str(link, "id"), Str(link, "status"), Excerpt(Str(link, "name"), 20),
        Str(link, "url"), who.empty() ? "" : "by " + who + " ", Excerpt(Str(link, "description"), 30));
}

bool HandleFriendList(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() > 1 || (words.size() == 1 && words[0] != "pending" && words[0] != "approved" && words[0] != "rejected"))
        return false;
    RunAsAdmin(handler, { .Op = &AdminListFriends, .Target = "/api/admin/friends?status=" + (words.empty() ? std::string() : words[0]) },
        [](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        boost::json::array const* links = Get(body, "friends").if_array();
        if (!links || links->empty())
            return handler.SendSysMessage("No links");
        std::string out;
        for (boost::json::value const& link : *links)
        {
            out += (out.empty() ? "" : "\n") + FriendLine(link);
            if (!Str(link, "message").empty())
                out += "\n        \"" + Excerpt(Str(link, "message"), 80) + "\"";
        }
        handler.SendSysMessage(out);
    });
    return true;
}

bool HandleFriendReview(CommandHandler& handler, std::string_view args, bool approve)
{
    std::vector<std::string> words = Words(args);
    Optional<uint32> id = IdWord(words);
    if (!id || (approve && words.size() != 1))
        return false;
    boost::json::object body{ { "status", approve ? "approved" : "rejected" } };
    if (!approve && words.size() > 1)
        body["rejectReason"] = Rest(words, 1);
    RunAsAdmin(handler, ItemCall("friends", &AdminUpdateFriend, *id, std::move(body)),
        Saying(Rooftree::StringFormat("Link {} {}, the applicant is mailed", *id, approve ? "approved" : "rejected")));
    return true;
}

bool HandleFriendApprove(CommandHandler& handler, std::string_view args) { return HandleFriendReview(handler, args, true); }
bool HandleFriendReject(CommandHandler& handler, std::string_view args) { return HandleFriendReview(handler, args, false); }

bool HandleFriendAdd(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() < 2)
        return false;
    boost::json::object body{ { "name", words[0] }, { "url", words[1] } };
    if (words.size() > 2)
        body["description"] = Rest(words, 2);
    RunAsAdmin(handler, { .Op = &AdminCreateFriend, .Target = "/api/admin/friends", .Body = std::move(body) },
        [](CommandHandler& handler, boost::json::value const& link, AuthInfo const&) { handler.SendSysMessage("Added\n" + FriendLine(link)); });
    return true;
}

bool HandleFriendEdit(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<uint32> id = IdWord(words);
    static constexpr std::string_view Fields[] = { "name", "url", "avatar", "description", "sort" };
    if (!id || words.size() < 3 || std::ranges::find(Fields, words[1]) == std::end(Fields))
        return false;
    std::string field = words[1], value = Rest(words, 2);

    // Name and url go together: the current ones first
    RunAsAdmin(handler, { .Op = &AdminListFriends, .Target = "/api/admin/friends?status=" },
        [id = *id, field, value](CommandHandler& handler, boost::json::value const& body, AuthInfo const& admin)
    {
        boost::json::value const* current = nullptr;
        if (boost::json::array const* links = Get(body, "friends").if_array())
            for (boost::json::value const& link : *links)
                if (Str(link, "id") == std::to_string(id))
                    current = &link;
        if (!current)
            return handler.SendSysMessage("Not found");

        boost::json::object update{ { "name", Str(*current, "name") }, { "url", Str(*current, "url") }, { "avatar", Str(*current, "avatar") },
            { "description", Str(*current, "description") } };
        if (field == "sort")
        {
            Optional<int32> sort = Rooftree::StringTo<int32>(value);
            if (!sort)
                return handler.SendSysMessage("sort is a number");
            update["sort"] = *sort;
        }
        else
            update[field] = value == "none" && field != "name" && field != "url" ? std::string() : value;
        Call(handler, admin, ItemCall("friends", &AdminUpdateFriend, id, std::move(update)),
            [](CommandHandler& handler, boost::json::value const& link, AuthInfo const&) { handler.SendSysMessage("Saved\n" + FriendLine(link)); });
    });
    return true;
}

bool HandleFriendDelete(CommandHandler& handler, std::string_view args)
{
    Optional<uint32> id = IdWord(Words(args));
    if (!id)
        return false;
    RunAsAdmin(handler, ItemCall("friends", &AdminDeleteFriend, *id), Saying(Rooftree::StringFormat("Deleted link {}", *id)));
    return true;
}

// Worker: the projects as their page lists them
boost::json::array WorkerLoadProjects()
{
    boost::json::array projects;
    if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_PROJECTS)))
    {
        do
        {
            Field* fields = rows->Fetch();
            projects.push_back({ { "id", fields[0].GetUInt32() }, { "name", fields[1].GetString() }, { "url", fields[2].GetString() },
                { "repoUrl", fields[3].GetString() }, { "icon", fields[4].GetString() }, { "descriptionZh", fields[5].GetString() },
                { "descriptionEn", fields[6].GetString() }, { "sort", fields[7].GetInt32() } });
        } while (rows->NextRow());
    }
    return projects;
}

std::string ProjectLine(boost::json::value const& project)
{
    return Rooftree::StringFormat("{:>4}  {:<24} {:<36} {}", Str(project, "id"), Excerpt(Str(project, "name"), 24),
        Str(project, "url").empty() ? Str(project, "repoUrl") : Str(project, "url"), Excerpt(Str(project, "descriptionZh"), 40));
}

Rooftree::Task<void> ListProjects(CommandHandler& handler)
{
    boost::json::array projects = co_await handler.GetSession().OnWorker([] { return WorkerLoadProjects(); });
    std::string out;
    for (boost::json::value const& project : projects)
        out += (out.empty() ? "" : "\n") + ProjectLine(project);
    handler.SendSysMessage(out.empty() ? "No projects" : out);
}

bool HandleProjectList(CommandHandler& handler, std::string_view /*args*/)
{
    handler.Spawn(ListProjects(handler));
    return true;
}

bool HandleProjectAdd(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.empty() || words.size() > 2)
        return false;
    boost::json::object body{ { "name", words[0] } };
    if (words.size() == 2)
        body["url"] = words[1];
    RunAsAdmin(handler, { .Op = &AdminCreateProject, .Target = "/api/admin/projects", .Body = std::move(body) },
        [](CommandHandler& handler, boost::json::value const& project, AuthInfo const&) { handler.SendSysMessage("Added\n" + ProjectLine(project)); });
    return true;
}

// Every field at once, as the page saves it: the current ones first
Rooftree::Task<void> EditProject(CommandHandler& handler, uint32 id, std::string key, std::string value)
{
    boost::json::array projects = co_await handler.GetSession().OnWorker([] { return WorkerLoadProjects(); });
    auto current = std::ranges::find_if(projects, [id](boost::json::value const& project) { return Str(project, "id") == std::to_string(id); });
    if (current == projects.end())
        co_return handler.SendSysMessage("Not found");
    boost::json::object update = current->as_object();
    update.erase("id");
    if (key == "sort")
    {
        Optional<int32> sort = Rooftree::StringTo<int32>(value);
        if (!sort)
            co_return handler.SendSysMessage("sort is a number");
        update["sort"] = *sort;
    }
    else
        update[key] = value == "none" && key != "name" ? std::string() : value;
    RunAsAdmin(handler, ItemCall("projects", &AdminUpdateProject, id, std::move(update)),
        [](CommandHandler& handler, boost::json::value const& project, AuthInfo const&) { handler.SendSysMessage("Saved\n" + ProjectLine(project)); });
}

bool HandleProjectEdit(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<uint32> id = IdWord(words);
    static constexpr std::pair<std::string_view, std::string_view> Fields[] = { { "name", "name" }, { "url", "url" }, { "repo", "repoUrl" },
        { "icon", "icon" }, { "zh", "descriptionZh" }, { "en", "descriptionEn" }, { "sort", "sort" } };
    auto field = std::ranges::find(Fields, std::string_view(words.size() > 1 ? words[1] : ""), &std::pair<std::string_view, std::string_view>::first);
    if (!id || words.size() < 3 || field == std::end(Fields))
        return false;
    std::string key(field->second), value = Rest(words, 2);

    handler.Spawn(EditProject(handler, *id, std::move(key), std::move(value)));
    return true;
}

bool HandleProjectDelete(CommandHandler& handler, std::string_view args)
{
    Optional<uint32> id = IdWord(Words(args));
    if (!id)
        return false;
    RunAsAdmin(handler, ItemCall("projects", &AdminDeleteProject, *id), Saying(Rooftree::StringFormat("Deleted project {}", *id)));
    return true;
}

// ---- uploads

bool HandleUpload(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 1)
        return false;
    std::string name = std::filesystem::path(words[0]).filename().string();
    RunAsAdmin(handler, { words[0] }, {}, [name](CommandHandler&, Prepared& prepared) -> Optional<CallSpec>
    {
        // The page's form: the file, its size and placeholder left out (the page shows it without)
        std::string boundary = "----blog-console-" + Rooftree::Server::GenerateToken(16);
        std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + name
            + "\"\r\nContent-Type: application/octet-stream\r\n\r\n" + prepared.Files[0] + "\r\n--" + boundary + "--\r\n";
        return CallSpec{ .Op = &AdminUpload, .Target = "/api/admin/uploads", .RawBody = std::move(body),
            .ContentType = "multipart/form-data; boundary=" + boundary };
    }, [](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        handler.PSendSysMessage("Uploaded {} ({}, {}): use it as {}", Str(body, "url"), Str(body, "mime"),
            FormatSize(Rooftree::StringTo<uint64>(Str(body, "size")).value_or(0)), Str(body, "url"));
    });
    return true;
}

// ---- accounts

CallSpec UserCall(ApiOp op, uint32 id, std::string suffix = {}, Optional<boost::json::object> body = {})
{
    return { .Op = op, .Target = Rooftree::StringFormat("/api/admin/users/{}{}", id, suffix), .Path = { { "id", std::to_string(id) } }, .Body = std::move(body) };
}

// The account of the first word, then make with it
bool WithAccount(CommandHandler& handler, std::string account, std::function<Optional<CallSpec>(CommandHandler&, UserRow const&)> make, Print print)
{
    RunAsAdmin(handler, {}, { { Lookup::Account, std::move(account) } }, [make = std::move(make)](CommandHandler& handler, Prepared& prepared)
    {
        return make(handler, prepared.Accounts[0]);
    }, std::move(print));
    return true;
}

void PrintAccount(CommandHandler& handler, boost::json::value const& user)
{
    std::string out = Rooftree::StringFormat("{} ({}), {} <{}>{}, {}", Str(user, "username"), Str(user, "id"), Str(user, "displayName"), Str(user, "email"),
        Get(user, "emailVerified").is_bool() && Get(user, "emailVerified").as_bool() ? "" : " (not confirmed)", Str(user, "role"));
    if (!Str(user, "bannedUntil").empty())
        out += Rooftree::StringFormat("\n  banned until {}{}", ShortTime(Str(user, "bannedUntil")), Str(user, "banReason").empty() ? "" : ": " + Str(user, "banReason"));
    std::string denied = Join(Get(user, "denied"));
    out += Rooftree::StringFormat("\n  since {}, last seen {}, {} comments, two-step login {}, may not: {}", ShortTime(Str(user, "createdAt")),
        Str(user, "lastSeenAt").empty() ? "never" : ShortTime(Str(user, "lastSeenAt")), Str(user, "commentCount"), Get(user, "totpEnabled").is_bool() && Get(user, "totpEnabled").as_bool() ? "on" : "off",
        denied.empty() ? "-" : denied);
    boost::json::value const& drive = Get(user, "drive");
    if (Get(drive, "enabled").is_bool() && Get(drive, "enabled").as_bool())
    {
        uint64 quota = Rooftree::StringTo<uint64>(Str(drive, Str(drive, "quota").empty() ? "defaultQuota" : "quota")).value_or(0);
        out += Rooftree::StringFormat("\n  drive: {} of {}{}, trash {} days", FormatSize(Rooftree::StringTo<uint64>(Str(drive, "used")).value_or(0)),
            FormatSize(quota), Str(drive, "quota").empty() ? " (default)" : "", Str(drive, Str(drive, "trashDays").empty() ? "defaultTrashDays" : "trashDays"));
    }
    else
        out += "\n  drive: off";
    out += Rooftree::StringFormat("\n  writing: {}", Get(user, "write").is_bool() && Get(user, "write").as_bool() ? "on" : "off");
    if (boost::json::array const* sessions = Get(user, "sessions").if_array())
    {
        out += Rooftree::StringFormat("\n  {} logged in devices:", sessions->size());
        for (boost::json::value const& session : *sessions)
            out += Rooftree::StringFormat("\n    {}  {:<15} last {}  {}", Str(session, "id").substr(0, 12), Str(session, "ip"),
                ShortTime(Str(session, "lastSeenAt")), Excerpt(Str(session, "userAgent"), 50));
    }
    handler.SendSysMessage(out);
}

bool HandleAccountShow(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 1)
        return false;
    return WithAccount(handler, words[0], [](CommandHandler&, UserRow const& user) { return UserCall(&AdminGetUser, user.Id); },
        [](CommandHandler& handler, boost::json::value const& user, AuthInfo const&) { PrintAccount(handler, user); });
}

bool HandleAccountCreate(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() < 2)
        return false;
    boost::json::object body{ { "username", words[0] }, { "email", words[1] }, { "displayName", words.size() > 2 ? Rest(words, 2) : words[0] } };
    std::string username = words[0], email = words[1];
    RunAsAdmin(handler, { .Op = &AdminCreateUser, .Target = "/api/admin/users", .Body = std::move(body) },
        [username, email](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        handler.PSendSysMessage("Created {} ({}), a mail to {} lets it choose a password", username, Str(body, "id"), email);
    });
    return true;
}

bool HandleAccountRole(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 2 || (words[1] != "user" && words[1] != "moderator"))
        return false;
    std::string role = words[1];
    return WithAccount(handler, words[0], [role](CommandHandler&, UserRow const& user)
    {
        return UserCall(&AdminUpdateUser, user.Id, {}, boost::json::object{ { "role", role } });
    }, [](CommandHandler& handler, boost::json::value const& user, AuthInfo const&) { handler.PSendSysMessage("{} is a {} now", Str(user, "username"), Str(user, "role")); });
}

bool HandleAccountDenied(CommandHandler& handler, std::string_view args, bool deny)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 2 || std::ranges::find(DeniableActions, std::string_view(words[1])) == std::end(DeniableActions))
        return false;
    std::string action = words[1];
    return WithAccount(handler, words[0], [action, deny](CommandHandler&, UserRow const& user)
    {
        std::vector<std::string> denied = user.Denied;
        std::erase(denied, action);
        if (deny)
            denied.push_back(action);
        boost::json::array list;
        for (std::string const& item : denied)
            list.emplace_back(item);
        return UserCall(&AdminUpdateUser, user.Id, {}, boost::json::object{ { "denied", std::move(list) } });
    }, [](CommandHandler& handler, boost::json::value const& user, AuthInfo const&)
    {
        std::string denied = Join(Get(user, "denied"));
        handler.PSendSysMessage("{} may not: {}", Str(user, "username"), denied.empty() ? "-" : denied);
    });
}

bool HandleAccountDeny(CommandHandler& handler, std::string_view args) { return HandleAccountDenied(handler, args, true); }
bool HandleAccountAllow(CommandHandler& handler, std::string_view args) { return HandleAccountDenied(handler, args, false); }

bool HandleAccountWrite(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    Optional<bool> enabled = words.size() == 2 ? OnOff(words[1]) : Optional<bool>();
    if (!enabled)
        return false;
    return WithAccount(handler, words[0], [enabled = *enabled](CommandHandler&, UserRow const& user)
    {
        return UserCall(&AdminSetUserWrite, user.Id, "/write", boost::json::object{ { "enabled", enabled } });
    }, [](CommandHandler& handler, boost::json::value const& user, AuthInfo const&) { PrintAccount(handler, user); });
}

bool HandleAccountDrive(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() < 2 || words.size() > 4)
        return false;
    Optional<bool> enabled = OnOff(words[1]);
    if (!enabled)
        return false;

    boost::json::object body{ { "enabled", *enabled } };
    if (words.size() > 2)
    {
        if (words[2] == "default")
            body["quota"] = nullptr;
        else if (Optional<uint64> size = ParseSize(words[2]))
            body["quota"] = *size;
        else
            return false;
    }
    if (words.size() > 3)
    {
        if (words[3] == "default")
            body["trashDays"] = nullptr;
        else if (Optional<uint32> days = Rooftree::StringTo<uint32>(words[3]))
            body["trashDays"] = *days;
        else
            return false;
    }

    return WithAccount(handler, words[0], [body](CommandHandler&, UserRow const& user) mutable
    {
        // A size left out keeps the account's
        if (!body.contains("quota"))
            body["quota"] = user.DriveQuota ? boost::json::value(*user.DriveQuota) : boost::json::value(nullptr);
        if (!body.contains("trashDays"))
            body["trashDays"] = user.DriveTrashDays ? boost::json::value(*user.DriveTrashDays) : boost::json::value(nullptr);
        return UserCall(&AdminSetUserDrive, user.Id, "/drive", std::move(body));
    }, [](CommandHandler& handler, boost::json::value const& user, AuthInfo const&) { PrintAccount(handler, user); });
}

bool HandleAccountKick(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.empty() || words.size() > 2)
        return false;
    if (words.size() == 1)
        return WithAccount(handler, words[0], [](CommandHandler&, UserRow const& user) { return UserCall(&AdminDeleteSessions, user.Id, "/sessions"); },
            Saying(words[0] + " is logged out everywhere"));

    // A device by the start of its id, as 'account show' prints it
    std::string prefix = words[1];
    return WithAccount(handler, words[0], [](CommandHandler&, UserRow const& user) { return UserCall(&AdminGetUser, user.Id); },
        [prefix](CommandHandler& handler, boost::json::value const& user, AuthInfo const& admin)
    {
        std::vector<std::string> matches;
        if (boost::json::array const* sessions = Get(user, "sessions").if_array())
            for (boost::json::value const& session : *sessions)
                if (Str(session, "id").starts_with(prefix))
                    matches.push_back(Str(session, "id"));
        if (matches.size() != 1)
            return handler.PSendSysMessage("{} devices start with {}, 'account show' lists them", matches.size(), prefix);
        uint32 id = Rooftree::StringTo<uint32>(Str(user, "id")).value_or(0);
        CallSpec spec = UserCall(&AdminDeleteSession, id, "/sessions/" + matches[0]);
        spec.Path.emplace_back("session", matches[0]);
        Call(handler, admin, std::move(spec), Saying("That device is logged out"));
    });
}

bool HandleAccountResetMail(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    if (words.size() != 1)
        return false;
    return WithAccount(handler, words[0], [](CommandHandler&, UserRow const& user) { return UserCall(&AdminMailPasswordReset, user.Id, "/password-reset"); },
        Saying("A link to choose a new password is mailed to " + words[0]));
}

// ---- the server's numbers

bool HandleOnline(CommandHandler& handler, std::string_view /*args*/)
{
    RunAsAdmin(handler, { .Op = &AdminOnline, .Target = "/api/admin/online" }, [](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        handler.PSendSysMessage("{} pages open, {} accounts logged in", Str(body, "viewers"), Str(body, "users"));
    });
    return true;
}

bool HandleStats(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Words(args);
    uint32 days = words.empty() ? 7 : Rooftree::StringTo<uint32>(words[0]).value_or(0);
    if (!days || words.size() > 1)
        return false;
    RunAsAdmin(handler, { .Op = &AdminStats, .Target = Rooftree::StringFormat("/api/admin/stats?days={}", days) },
        [](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        // Visitors are counted per day: a sum of days counts a reader once a day
        std::string out;
        uint64 views = 0, visitors = 0;
        if (boost::json::array const* days = Get(body, "days").if_array())
            for (boost::json::value const& day : *days)
            {
                views += Rooftree::StringTo<uint64>(Str(day, "views")).value_or(0);
                visitors += Rooftree::StringTo<uint64>(Str(day, "visitors")).value_or(0);
                out += Rooftree::StringFormat("\n  {}{}  {:>6} views  {:>5} visitors", Str(day, "day"), Str(day, "day") == Str(body, "today") ? " (today)" : "",
                    Str(day, "views"), Str(day, "visitors"));
            }
        out = Rooftree::StringFormat("{} views, {} visitors a day added up:", views, visitors) + out;
        if (boost::json::array const* pages = Get(body, "pages").if_array(); pages && !pages->empty())
        {
            out += "\nRead most:";
            for (std::size_t i = 0; i < pages->size() && i < 10; ++i)
                out += Rooftree::StringFormat("\n  {:>6}  {}", Str((*pages)[i], "views"), Str((*pages)[i], "path"));
        }
        if (boost::json::array const* referrers = Get(body, "referrers").if_array(); referrers && !referrers->empty())
        {
            out += "\nCame from:";
            for (std::size_t i = 0; i < referrers->size() && i < 10; ++i)
                out += Rooftree::StringFormat("\n  {:>6}  {}", Str((*referrers)[i], "views"), Str((*referrers)[i], "host"));
        }
        handler.SendSysMessage(out);
    });
    return true;
}

bool HandleStatus(CommandHandler& handler, std::string_view /*args*/)
{
    // Only now, not the day's samples
    int64 now = int64(std::chrono::duration_cast<Seconds>(SystemTimePoint::clock::now().time_since_epoch()).count());
    RunAsAdmin(handler, { .Op = &AdminStatus, .Target = Rooftree::StringFormat("/api/status?since={}", now) },
        [](CommandHandler& handler, boost::json::value const& body, AuthInfo const&)
    {
        boost::json::value const& now = Get(body, "now");
        int64 uptime = Rooftree::StringTo<int64>(Str(now, "uptime")).value_or(0);
        handler.PSendSysMessage("Up {}d {}h {}m, {} readers on {} pages, {} requests a minute\n"
            "Update {} us on average, {} us at most; {} MiB, CPU {}% of {} cores, load {}, host memory {}%, disk {}% free",
            uptime / 86400, uptime / 3600 % 24, uptime / 60 % 60, Str(now, "readers"), Str(now, "pages"), Str(now, "requests"),
            Str(now, "updateAvgUs"), Str(now, "updateMaxUs"), Str(now, "memoryMiB"), Str(now, "cpuPercent"), Str(now, "cores"), Str(now, "load"),
            Str(now, "hostMemoryPercent"), Str(now, "diskFreePercent"));
    });
    return true;
}

Rooftree::Server::Command Cmd(std::string name, std::string help, Rooftree::Server::CommandFunction handler)
{
    return { .Name = std::move(name), .Help = std::move(help), .Handler = std::move(handler) };
}
}

bool ReadConsoleFile(std::string const& path, std::string& data, std::string& error)
{
    std::error_code code;
    std::uintmax_t size = std::filesystem::file_size(path, code);
    if (code)
    {
        error = Rooftree::StringFormat("{}: {}", path, code.message());
        return false;
    }
    if (size > MaxConsoleFile)
    {
        error = Rooftree::StringFormat("{}: larger than {} MB", path, MaxConsoleFile / 1024 / 1024);
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    data.resize(size);
    if (!file || !file.read(data.data(), std::streamsize(size)))
    {
        error = Rooftree::StringFormat("{}: can't be read", path);
        return false;
    }
    return true;
}

std::vector<Rooftree::Server::Command> GetConsoleAdminCommands()
{
    std::string const files = "\n<file> is a path on the server (in the container with Docker): docker compose cp puts one there.";
    return
    {
        {
            .Name = "post",
            .Help = "Posts, by slug or id",
            .Subcommands =
            {
                Cmd("list", "Syntax: post list [draft|published|review] [page]\nreview: the ones writers asked to be published.", &HandlePostList),
                Cmd("decline", "Syntax: post decline <post>\nNo to a writer asking for it to be published.", &HandlePostDecline),
                Cmd("show", "Syntax: post show <post>\nIts state, languages, tags.", &HandlePostShow),
                Cmd("revisions", "Syntax: post revisions <post> [<version>]\nIts versions, each publication and each one saved, newest first; or one in full.",
                    &HandlePostRevisions),
                Cmd("save-version", "Syntax: post save-version <post> [note]\nIts working copy as a version, of each language changed since its last one.",
                    &HandlePostSaveVersion),
                Cmd("restore", "Syntax: post restore <post> <version>\nThat version back in the working copy; post publish gives it to readers.",
                    &HandlePostRestore),
                Cmd("editors", "Syntax: post editors <post>\nIts owner, who else edits it and what more they may do.", &HandlePostEditors),
                {
                    .Name = "editor",
                    .Help = "Who else edits a post",
                    .Subcommands =
                    {
                        Cmd("add", "Syntax: post editor add <post> <account> [publish,settings,editors|none]\n"
                            "Invites it to edit the post's text, with those rights on top; again, its rights change.", &HandlePostEditorAdd),
                        Cmd("remove", "Syntax: post editor remove <post> <account>", &HandlePostEditorRemove),
                    },
                },
                Cmd("owner", "Syntax: post owner <post> <account>\nGives the post to the account; its owner before edits it, with every right.",
                    &HandlePostOwner),
                Cmd("create", "Syntax: post create <slug> <zh|en> <title> <file> [published]\nA new post from a Markdown file, a draft unless published." + files,
                    &HandlePostCreate),
                Cmd("edit", "Syntax: post edit <post> slug|status|lang|cover|comments|tags|publish-at <value>\n"
                    "tags: a,b,c or none; comments: on|off; cover: an address or none; publish-at: 2026-10-30 09:00+08:00 or none (a draft's time).",
                    &HandlePostEdit),
                {
                    .Name = "translation",
                    .Help = "A post's text in one language",
                    .Subcommands =
                    {
                        Cmd("set", "Syntax: post translation set <post> <zh|en> <title> <file> [summary]\nThe language's title, Markdown and summary, added or replaced."
                            + files, &HandlePostTranslationSet),
                        Cmd("delete", "Syntax: post translation delete <post> <zh|en>\nNot the original language.", &HandlePostTranslationDelete),
                    },
                },
                Cmd("publish", "Syntax: post publish <post>\nWhat its editors wrote goes to readers; the site is rebuilt, subscribers mailed (once).",
                    &HandlePostPublish),
                Cmd("unpublish", "Syntax: post unpublish <post>\nBack to a draft.", &HandlePostUnpublish),
                Cmd("delete", "Syntax: post delete <post>\nWith its comments, for good.", &HandlePostDelete),
            },
        },
        {
            .Name = "tag",
            .Help = "The posts' tags",
            .Subcommands =
            {
                Cmd("list", "Syntax: tag list", &HandleTagList),
                Cmd("add", "Syntax: tag add <slug> <name zh> <name en>", &HandleTagAdd),
                Cmd("edit", "Syntax: tag edit <slug> <name zh> <name en>", &HandleTagEdit),
                Cmd("delete", "Syntax: tag delete <slug>\nTaken off its posts too.", &HandleTagDelete),
            },
        },
        {
            .Name = "note",
            .Help = "Notes (the diary), by id",
            .Subcommands =
            {
                Cmd("list", "Syntax: note list [draft|published] [page]", &HandleNoteList),
                Cmd("show", "Syntax: note show <id>", &HandleNoteShow),
                Cmd("create", "Syntax: note create <zh|en> <title> <file> [published]\nA draft unless published." + files, &HandleNoteCreate),
                Cmd("edit", "Syntax: note edit <id> status|lang|mood|weather|cover|comments <value>", &HandleNoteEdit),
                {
                    .Name = "translation",
                    .Help = "A note's text in one language",
                    .Subcommands =
                    {
                        Cmd("set", "Syntax: note translation set <id> <zh|en> <title> <file>" + files, &HandleNoteTranslationSet),
                        Cmd("delete", "Syntax: note translation delete <id> <zh|en>", &HandleNoteTranslationDelete),
                    },
                },
                Cmd("publish", "Syntax: note publish <id>", &HandleNotePublish),
                Cmd("unpublish", "Syntax: note unpublish <id>", &HandleNoteUnpublish),
                Cmd("delete", "Syntax: note delete <id>", &HandleNoteDelete),
            },
        },
        {
            .Name = "thinking",
            .Help = "Thinking, the one-line posts, by id",
            .Subcommands =
            {
                Cmd("list", "Syntax: thinking list [page]", &HandleThinkingList),
                Cmd("add", "Syntax: thinking add <zh|en> <text>\nPublished at once.", &HandleThinkingAdd),
                Cmd("edit", "Syntax: thinking edit <id> <zh|en> <text>\nThe language's text, added or replaced.", &HandleThinkingEdit),
                Cmd("publish", "Syntax: thinking publish <id>", &HandleThinkingPublish),
                Cmd("unpublish", "Syntax: thinking unpublish <id>", &HandleThinkingUnpublish),
                Cmd("delete", "Syntax: thinking delete <id>", &HandleThinkingDelete),
            },
        },
        {
            .Name = "comment",
            .Help = "Comments, newest first",
            .Subcommands =
            {
                Cmd("list", "Syntax: comment list [visible|hidden|deleted] [orphaned] [before <id>]\n"
                    "orphaned: those on a passage their post no longer has, for the admin to decide.", &HandleCommentList),
                Cmd("keep", "Syntax: comment keep <id>\nA comment whose passage its post lost stays so: under \"the text changed\", no longer to decide.",
                    &HandleCommentKeep),
                Cmd("hide", "Syntax: comment hide <id>\nOnly the admin sees it then.", &HandleCommentHide),
                Cmd("show", "Syntax: comment show <id>\nVisible again.", &HandleCommentShow),
            },
        },
        {
            .Name = "chat",
            .Help = "Chat: the square and the readers' groups; direct chats only their two read",
            .Subcommands =
            {
                Cmd("groups", "Syntax: chat groups", &HandleChatGroups),
                Cmd("messages", "Syntax: chat messages square|<group id> [before <id>]\nNewest first, 50 at a time.", &HandleChatMessages),
                Cmd("delete", "Syntax: chat delete <message id>\nGone from every page showing it.", &HandleChatDelete),
                Cmd("clear", "Syntax: chat clear square|<group id>\nEvery message of the channel.", &HandleChatClear),
                Cmd("dissolve", "Syntax: chat dissolve <group id>\nThe group and its messages, for good.", &HandleChatDissolve),
            },
        },
        {
            .Name = "friend",
            .Help = "Friends' links: the author's and readers' applications",
            .Subcommands =
            {
                Cmd("list", "Syntax: friend list [pending|approved|rejected]", &HandleFriendList),
                Cmd("approve", "Syntax: friend approve <id>\nListed, the applicant mailed.", &HandleFriendApprove),
                Cmd("reject", "Syntax: friend reject <id> [reason]\nThe applicant mailed, with the reason.", &HandleFriendReject),
                Cmd("add", "Syntax: friend add <name> <url> [description]", &HandleFriendAdd),
                Cmd("edit", "Syntax: friend edit <id> name|url|avatar|description|sort <value>", &HandleFriendEdit),
                Cmd("delete", "Syntax: friend delete <id>", &HandleFriendDelete),
            },
        },
        {
            .Name = "project",
            .Help = "The projects page",
            .Subcommands =
            {
                Cmd("list", "Syntax: project list", &HandleProjectList),
                Cmd("add", "Syntax: project add <name> [url]\nThe rest with 'project edit'.", &HandleProjectAdd),
                Cmd("edit", "Syntax: project edit <id> name|url|repo|icon|zh|en|sort <value>\nzh, en: the description in that language.", &HandleProjectEdit),
                Cmd("delete", "Syntax: project delete <id>", &HandleProjectDelete),
            },
        },
        Cmd("upload", "Syntax: upload <file>\nAn image (png, jpeg, webp, gif, avif) for posts: its /uploads/ address." + files, &HandleUpload),
        Cmd("online", "Syntax: online\nPages open and accounts logged in now.", &HandleOnline),
        Cmd("stats", "Syntax: stats [days]\nReaders' views and visitors a day (7 days), the pages read most and where they came from.", &HandleStats),
        Cmd("status", "Syntax: status\nThe status page's numbers now.", &HandleStatus),
    };
}

std::vector<Rooftree::Server::Command> GetConsoleAccountAdminCommands()
{
    return
    {
        Cmd("show", "Syntax: account show <account>\nEverything the admin's page shows, its logged in devices too.", &HandleAccountShow),
        Cmd("create", "Syntax: account create <user name> <email> [display name]\nMailed a link to choose a password.", &HandleAccountCreate),
        Cmd("role", "Syntax: account role <account> user|moderator", &HandleAccountRole),
        Cmd("deny", "Syntax: account deny <account> comment|react|avatar|friend|chat\nTakes that from the account.", &HandleAccountDeny),
        Cmd("allow", "Syntax: account allow <account> comment|react|avatar|friend|chat\nGives it back.", &HandleAccountAllow),
        Cmd("write", "Syntax: account write <account> on|off\nLets it write posts of its own.", &HandleAccountWrite),
        Cmd("drive", "Syntax: account drive <account> on|off [size|default] [trash days|default]\nsize like 5G or 500M; left out, the account's stay.",
            &HandleAccountDrive),
        Cmd("kick", "Syntax: account kick <account> [device]\nLogs it out everywhere, or on one device (the start of its id, from 'account show').",
            &HandleAccountKick),
        Cmd("reset-mail", "Syntax: account reset-mail <account>\nMails it a link to choose a new password.", &HandleAccountResetMail),
    };
}
}
