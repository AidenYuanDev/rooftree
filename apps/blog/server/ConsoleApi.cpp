/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Like ConsoleCommands.cpp: the arguments are checked on the main thread; the account, its session,
 * the files and the request itself are a worker's, which waits for the response. One call at a time,
 * so the console never holds more than one of the workers the request itself needs.
 */

#include "ConsoleApi.h"
#include "ConsoleAdmin.h"
#include "Accounts.h"
#include "BlogDatabase.h"
#include "BlogWorld.h"
#include "HttpClient.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"
#include "WorldHttpService.h"
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <filesystem>
#include <fstream>

using Rooftree::Server::CommandHandler;
using Rooftree::Server::Session;

namespace Blog
{
namespace
{
Rooftree::Server::WorldHttpService const* ApiService = nullptr;
std::string ApiBaseUrl;
bool ApiCallRunning = false;

// The session lives for the one request, a long upload included
constexpr Seconds ApiSessionLifetime = 10min;
constexpr Seconds ApiTimeout = 5min;
constexpr std::string_view ApiUserAgent = "blog console";
// Printed at most, more goes to --out
constexpr std::size_t MaxPrintedBody = 256 * 1024;
constexpr std::size_t MaxPrintedResponse = 16 * 1024 * 1024;
constexpr std::size_t MaxSavedResponse = 512 * 1024 * 1024;

enum class ItemKind
{
    Text,       // key=text
    Json,       // key:=json
    FileText,   // key=@file
    FileJson,   // key:=@file
    FormFile    // key@file, --form only
};

struct Item
{
    std::string Key;
    ItemKind Kind;
    std::string Value;          // the text, the JSON or the file's path
};

enum class BodyKind
{
    None,
    Raw,        // JSON as typed
    File,       // @file
    Items,
    Form
};

struct ApiCall
{
    std::string Method;
    std::string Target;
    std::string Account;        // empty: the admin
    std::vector<std::pair<std::string, std::string>> Headers;
    std::string OutFile;
    BodyKind Body = BodyKind::None;
    std::string Raw;            // the JSON or the file's path
    std::vector<Item> Items;
    std::string Origin;
    std::string Url;
};

struct ApiResult
{
    std::string Error;
    std::string Username;
    std::string SessionHash;    // to drop from the auth cache
    Rooftree::Server::HttpClientResponse Response;
    bool Saved = false;
};

bool HasHeader(ApiCall const& call, std::string_view name)
{
    return std::ranges::any_of(call.Headers, [name](auto const& header)
    {
        return header.first.size() == name.size() && std::ranges::equal(header.first, name, [](char a, char b)
        {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    });
}

std::string GuessContentType(std::string const& path)
{
    std::string extension = std::filesystem::path(path).extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static constexpr std::pair<std::string_view, std::string_view> Types[] =
    {
        { ".json", "application/json" }, { ".md", "text/markdown; charset=utf-8" }, { ".txt", "text/plain; charset=utf-8" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".webp", "image/webp" },
        { ".gif", "image/gif" }, { ".avif", "image/avif" }, { ".svg", "image/svg+xml" }, { ".pdf", "application/pdf" },
    };
    for (auto const& [ext, type] : Types)
        if (extension == ext)
            return std::string(type);
    return "application/octet-stream";
}

bool ReadWholeFile(std::string const& path, std::string& data, std::string& error)
{
    return ReadConsoleFile(path, data, error);
}

// a.b.c=x: {"a": {"b": {"c": "x"}}}
bool SetNested(boost::json::object& root, std::string_view key, boost::json::value value, std::string& error)
{
    boost::json::object* object = &root;
    while (true)
    {
        std::size_t dot = key.find('.');
        std::string_view name = key.substr(0, dot);
        if (name.empty())
        {
            error = "an item with an empty name";
            return false;
        }
        if (dot == std::string_view::npos)
        {
            (*object)[name] = std::move(value);
            return true;
        }

        boost::json::value& child = (*object)[name];
        if (child.is_null())
            child = boost::json::object();
        else if (!child.is_object())
        {
            error = Rooftree::StringFormat("{} is set twice, as a value and as an object", name);
            return false;
        }
        object = &child.as_object();
        key = key.substr(dot + 1);
    }
}

// Worker: the request's body and its Content-Type
bool BuildBody(ApiCall& call, std::string& body, std::string& error)
{
    switch (call.Body)
    {
        case BodyKind::None:
            return true;
        case BodyKind::Raw:
            body = call.Raw;
            if (!HasHeader(call, "Content-Type"))
                call.Headers.emplace_back("Content-Type", "application/json");
            return true;
        case BodyKind::File:
            if (!ReadWholeFile(call.Raw, body, error))
                return false;
            if (!HasHeader(call, "Content-Type"))
                call.Headers.emplace_back("Content-Type", GuessContentType(call.Raw));
            return true;
        case BodyKind::Items:
        {
            boost::json::object object;
            for (Item const& item : call.Items)
            {
                std::string text = item.Value;
                if ((item.Kind == ItemKind::FileText || item.Kind == ItemKind::FileJson) && !ReadWholeFile(item.Value, text, error))
                    return false;

                boost::json::value value;
                if (item.Kind == ItemKind::Json || item.Kind == ItemKind::FileJson)
                {
                    boost::system::error_code code;
                    value = boost::json::parse(text, code);
                    if (code)
                    {
                        error = Rooftree::StringFormat("{}: not JSON ({})", item.Key, code.message());
                        return false;
                    }
                }
                else
                    value = boost::json::string(text);

                if (!SetNested(object, item.Key, std::move(value), error))
                    return false;
            }
            body = boost::json::serialize(object);
            if (!HasHeader(call, "Content-Type"))
                call.Headers.emplace_back("Content-Type", "application/json");
            return true;
        }
        case BodyKind::Form:
        {
            std::string boundary = "----blog-console-" + Rooftree::Server::GenerateToken(16);
            for (Item const& item : call.Items)
            {
                body += "--" + boundary + "\r\n";
                if (item.Kind == ItemKind::FormFile)
                {
                    std::string data;
                    if (!ReadWholeFile(item.Value, data, error))
                        return false;
                    body += Rooftree::StringFormat("Content-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\nContent-Type: {}\r\n\r\n",
                        item.Key, std::filesystem::path(item.Value).filename().string(), GuessContentType(item.Value));
                    body += data;
                }
                else
                    body += Rooftree::StringFormat("Content-Disposition: form-data; name=\"{}\"\r\n\r\n{}", item.Key, item.Value);
                body += "\r\n";
            }
            body += "--" + boundary + "--\r\n";
            call.Headers.emplace_back("Content-Type", "multipart/form-data; boundary=" + boundary);
            return true;
        }
    }
    return true;
}

// Worker: who, a session for it, the request, the session gone again
ApiResult WorkerCall(ApiCall call)
{
    ApiResult result;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(call.Account.empty() ? BLOG_SEL_FIRST_ADMIN : BLOG_SEL_USER_FOR_LOGIN);
    if (!call.Account.empty())
    {
        stmt->setString(0, call.Account);
        stmt->setString(1, call.Account);
    }
    PreparedQueryResult account = BlogDatabase.Query(stmt);
    if (!account)
    {
        result.Error = call.Account.empty() ? "There is no admin yet: blog --create-admin"
            : Rooftree::StringFormat("There is no account {}, 'lookup account' finds one by a part of it", call.Account);
        return result;
    }
    UserRow user = UserRow::FromFields(account->Fetch());
    result.Username = user.Username;

    std::string body;
    if (!BuildBody(call, body, result.Error))
        return result;

    NewAuthSession session = WorkerCreateAuthSession(user.Id, ApiSessionLifetime, "127.0.0.1", std::string(ApiUserAgent));
    result.SessionHash = session.Hash;

    Rooftree::Server::HttpClientRequest request{ .Method = call.Method, .Url = call.Url + call.Target, .Headers = call.Headers,
        .Body = std::move(body), .Timeout = ApiTimeout, .MaxResponseSize = call.OutFile.empty() ? MaxPrintedResponse : MaxSavedResponse };
    request.Headers.emplace_back("Cookie", sWorld->GetSettings().CookieName + "=" + session.Token);
    request.Headers.emplace_back("X-CSRF-Token", session.CsrfToken);
    request.Headers.emplace_back("Origin", call.Origin);
    request.Headers.emplace_back("User-Agent", std::string(ApiUserAgent));
    result.Response = Rooftree::Server::HttpFetch(request);

    stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_SESSION);
    stmt->setString(0, session.Hash);
    BlogDatabase.DirectExecute(stmt);

    if (!call.OutFile.empty() && result.Response.Status)
    {
        std::ofstream out(call.OutFile, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(result.Response.Body.data(), std::streamsize(result.Response.Body.size())))
            result.Error = Rooftree::StringFormat("{}: can't be written", call.OutFile);
        else
            result.Saved = true;
    }
    return result;
}

void PrettyJson(boost::json::value const& value, std::string& out, std::size_t indent)
{
    auto newline = [&](std::size_t depth) { out += '\n'; out.append(depth * 2, ' '); };
    if (value.is_object() && !value.as_object().empty())
    {
        out += '{';
        bool first = true;
        for (auto const& [key, child] : value.as_object())
        {
            out += first ? "" : ",";
            first = false;
            newline(indent + 1);
            out += boost::json::serialize(boost::json::string(key)) + ": ";
            PrettyJson(child, out, indent + 1);
        }
        newline(indent);
        out += '}';
    }
    else if (value.is_array() && !value.as_array().empty())
    {
        out += '[';
        bool first = true;
        for (boost::json::value const& child : value.as_array())
        {
            out += first ? "" : ",";
            first = false;
            newline(indent + 1);
            PrettyJson(child, out, indent + 1);
        }
        newline(indent);
        out += ']';
    }
    else
        out += boost::json::serialize(value);
}

void PrintResult(CommandHandler& handler, ApiCall const& call, ApiResult const& result)
{
    if (!result.Error.empty())
        return handler.SendSysMessage(result.Error);

    Rooftree::Server::HttpClientResponse const& response = result.Response;
    if (!response.Status)
        return handler.PSendSysMessage("{} {}: no response, {}", call.Method, call.Target, response.Error);

    handler.PSendSysMessage("{} {} as {}: {}", call.Method, call.Target, result.Username, response.Status);
    if (result.Saved)
        return handler.PSendSysMessage("{} bytes of {} saved to {}", response.Body.size(), response.ContentType, call.OutFile);
    if (response.Body.empty())
        return;

    std::string_view type = response.ContentType;
    if (type.starts_with("application/json"))
    {
        boost::system::error_code code;
        boost::json::value value = boost::json::parse(response.Body, code);
        if (!code)
        {
            std::string out;
            PrettyJson(value, out, 0);
            if (out.size() <= MaxPrintedBody)
                return handler.SendSysMessage(out);
        }
    }

    bool text = type.starts_with("text/") || type.starts_with("application/json") || type.find("xml") != std::string_view::npos;
    if (!text || response.Body.size() > MaxPrintedBody)
        return handler.PSendSysMessage("{} bytes of {}, --out <file> saves them", response.Body.size(), type.empty() ? "unknown type" : type);
    handler.SendSysMessage(response.Body);
}

// The next word of args (a "quoted" one may have spaces), args moved past it
std::string TakeWord(std::string_view& args)
{
    std::size_t begin = args.find_first_not_of(' ');
    if (begin == std::string_view::npos)
    {
        args = {};
        return {};
    }
    args.remove_prefix(begin);

    std::string word;
    if (args.front() == '"')
    {
        std::size_t end = args.find('"', 1);
        word = std::string(args.substr(1, end == std::string_view::npos ? std::string_view::npos : end - 1));
        args.remove_prefix(end == std::string_view::npos ? args.size() : end + 1);
        return word;
    }

    std::size_t end = args.find(' ');
    word = std::string(args.substr(0, end));
    args.remove_prefix(end == std::string_view::npos ? args.size() : end);
    return word;
}

// Words like a shell's: quotes anywhere in one keep its spaces (key="a b"), 'single' ones keep
// everything as it is (tags:='["a", "b"]'), \" in double ones is a quote
std::vector<std::string> SplitItems(std::string_view text)
{
    std::vector<std::string> words;
    std::string word;
    bool inWord = false;
    char quote = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (quote == '"' && c == '\\' && i + 1 < text.size() && (text[i + 1] == '"' || text[i + 1] == '\\'))
            word += text[++i];
        else if (quote ? c == quote : (c == '"' || c == '\''))
        {
            quote = quote ? 0 : c;
            inWord = true;
        }
        else if (c == ' ' && !quote)
        {
            if (inWord)
                words.push_back(std::move(word));
            word.clear();
            inWord = false;
        }
        else
        {
            word += c;
            inWord = true;
        }
    }
    if (inWord)
        words.push_back(std::move(word));
    return words;
}

bool ParseItems(CommandHandler& handler, std::string_view text, bool form, std::vector<Item>& items)
{
    for (std::string const& word : SplitItems(text))
    {
        std::size_t at = word.find_first_of(form ? ":=@" : ":=");
        Item item;
        if (at != std::string::npos && word[at] == ':' && word.compare(at, 2, ":=") == 0 && !form)
        {
            item.Key = word.substr(0, at);
            item.Value = word.substr(at + 2);
            item.Kind = item.Value.starts_with('@') ? ItemKind::FileJson : ItemKind::Json;
        }
        else if (at != std::string::npos && word[at] == '=')
        {
            item.Key = word.substr(0, at);
            item.Value = word.substr(at + 1);
            item.Kind = !form && item.Value.starts_with('@') ? ItemKind::FileText : ItemKind::Text;
        }
        else if (at != std::string::npos && word[at] == '@')
        {
            item.Key = word.substr(0, at);
            item.Value = word.substr(at + 1);
            item.Kind = ItemKind::FormFile;
        }
        else
        {
            handler.PSendSysMessage("{}: an item is key=text, key:=json, key=@file or key:=@file{}", word, form ? " (with --form: key=text or key@file)" : "");
            return false;
        }

        if (item.Key.empty())
        {
            handler.PSendSysMessage("{}: an item needs a name", word);
            return false;
        }
        if (item.Kind == ItemKind::FileText || item.Kind == ItemKind::FileJson || item.Kind == ItemKind::FormFile)
            item.Value.erase(0, item.Kind == ItemKind::FormFile ? 0 : 1);
        items.push_back(std::move(item));
    }
    return true;
}

Rooftree::Task<void> CallApi(CommandHandler& handler, ApiCall call)
{
    ApiResult result = co_await handler.GetSession().OnWorker([call]() mutable { return WorkerCall(std::move(call)); });
    ApiCallRunning = false;
    // Deleted already, the cache would still let it in for a while
    if (!result.SessionHash.empty())
        sWorld->ForgetAuth(result.SessionHash);
    if (result.Response.Status)
        RT_LOG_INFO("blog.admin", "Console: {} {} as {}: {}", call.Method, call.Target, result.Username, result.Response.Status);
    PrintResult(handler, call, result);
}

bool HandleApiCall(CommandHandler& handler, std::string_view method, std::string_view args)
{
    if (!ApiService)
    {
        handler.SendSysMessage("The API isn't running");
        return true;
    }

    ApiCall call;
    call.Method = std::string(method);
    call.Target = TakeWord(args);
    if (!call.Target.starts_with('/'))
        return false;

    bool form = false;
    while (true)
    {
        std::string_view rest = args.substr(std::min(args.size(), args.find_first_not_of(' ')));
        if (rest.starts_with("--as ") || rest == "--as")
        {
            args = rest.substr(4);
            call.Account = TakeWord(args);
            if (call.Account.empty())
                return false;
        }
        else if (rest.starts_with("--out ") || rest == "--out")
        {
            args = rest.substr(5);
            call.OutFile = TakeWord(args);
            if (call.OutFile.empty())
                return false;
        }
        else if (rest.starts_with("-H ") || rest == "-H")
        {
            args = rest.substr(2);
            std::string header = TakeWord(args);
            std::size_t colon = header.find(':');
            if (colon == std::string::npos || colon == 0)
            {
                handler.PSendSysMessage("-H \"{}\": a header is \"Name: value\"", header);
                return true;
            }
            std::size_t value = header.find_first_not_of(' ', colon + 1);
            call.Headers.emplace_back(header.substr(0, colon), value == std::string::npos ? std::string() : header.substr(value));
        }
        else if (rest.starts_with("--form ") || rest == "--form")
        {
            args = rest.substr(6);
            form = true;
        }
        else
        {
            args = rest;
            break;
        }
    }

    while (!args.empty() && args.back() == ' ')
        args.remove_suffix(1);

    if (args.empty())
        call.Body = form ? BodyKind::Form : BodyKind::None;
    else if (!form && (args.front() == '{' || args.front() == '['))
    {
        boost::system::error_code code;
        boost::json::parse(args, code);
        if (code)
        {
            handler.PSendSysMessage("The body isn't JSON: {}", code.message());
            return true;
        }
        call.Body = BodyKind::Raw;
        call.Raw = std::string(args);
    }
    else if (!form && args.front() == '@' && args.find(' ') == std::string_view::npos)
    {
        call.Body = BodyKind::File;
        call.Raw = std::string(args.substr(1));
    }
    else
    {
        call.Body = form ? BodyKind::Form : BodyKind::Items;
        if (!ParseItems(handler, args, form, call.Items))
            return true;
    }

    if (ApiCallRunning)
    {
        handler.SendSysMessage("Another api call is running, one at a time");
        return true;
    }

    BlogSettings const& settings = sWorld->GetSettings();
    call.Origin = settings.AllowedOrigins.empty() ? settings.PublicOrigin : settings.AllowedOrigins.front();
    call.Url = ApiBaseUrl;

    ApiCallRunning = true;
    handler.Spawn(CallApi(handler, std::move(call)));
    return true;
}

bool HandleApiRoutes(CommandHandler& handler, std::string_view args)
{
    if (!ApiService)
    {
        handler.SendSysMessage("The API isn't running");
        return true;
    }

    std::string filter(args.substr(std::min(args.size(), args.find_first_not_of(' '))));
    std::string out;
    std::size_t count = 0;
    for (Rooftree::Server::WorldHttpService::RouteInfo const& route : ApiService->GetRoutes())
    {
        if (!filter.empty() && route.Path.find(filter) == std::string::npos)
            continue;
        std::string verb(boost::beast::http::to_string(route.Method));
        out += Rooftree::StringFormat("{}{:<7} {:<45} {}", out.empty() ? "" : "\n", verb, route.Path, route.MessageType);
        ++count;
    }
    handler.SendSysMessage(count ? out : "No route has " + filter);
    return true;
}

std::string const CallHelp =
    " <path> [--as <account>] [-H \"Name: value\"] [--out <file>] [--form] [body]\n"
    "The request as the admin (or --as the account), through the same handler as the web's.\n"
    "body: {\"a\": 1} as it is; @file the file's bytes; or items: key=text key:=json key=@file key:=@file\n"
    "(a.b=x nests; with --form multipart: key=text key@file). Files are the server's. Example:\n"
    "  api post /api/admin/posts slug=hello originalLang=zh translations.zh.title=你好 translations.zh.body=@/tmp/hello.md";
}

void SetConsoleApi(Rooftree::Server::WorldHttpService const* service, std::string bindIp, uint16 port)
{
    ApiService = service;
    // Whatever it listens on, the host reaches it on its loopback, which is never redirected to https
    if (bindIp.empty() || bindIp == "0.0.0.0")
        bindIp = "127.0.0.1";
    else if (bindIp == "::")
        bindIp = "::1";
    if (bindIp.find(':') != std::string::npos)
        bindIp = "[" + bindIp + "]";
    ApiBaseUrl = Rooftree::StringFormat("http://{}:{}", bindIp, port);
}

Rooftree::Server::Command GetConsoleApiCommand()
{
    auto call = [](std::string name, std::string method)
    {
        return Rooftree::Server::Command
        {
            .Name = name,
            .Help = "Syntax: api " + name + CallHelp,
            .Handler = [method](CommandHandler& handler, std::string_view args) { return HandleApiCall(handler, method, args); },
        };
    };

    return
    {
        .Name = "api",
        .Help = "Every route of the API, as the admin or another account ('api routes' lists them)",
        .Subcommands =
        {
            { .Name = "routes", .Help = "Syntax: api routes [text]\nThe routes (method, path, AntiDOS type), those whose path has text.", .Handler = &HandleApiRoutes },
            call("get", "GET"),
            call("post", "POST"),
            call("put", "PUT"),
            call("patch", "PATCH"),
            call("delete", "DELETE"),
        },
    };
}
}
