/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Collab.h"
#include "BlogDatabase.h"
#include "Log.h"
#include "PostAccess.h"
#include "PreparedStatement.h"
#include "StringConvert.h"
#include "WorldWebSocket.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <utf8.h>

namespace Blog
{
namespace
{
// Changes kept to bring a page that was away up to date; older, it gets the whole text again
constexpr std::size_t KeptChanges = 2000;
// Changes in one push
constexpr std::size_t MaxChangesPerPush = 100;
constexpr Milliseconds SaveInterval = 1s;
constexpr Seconds ForgetAfter = 60s;
constexpr std::size_t MaxClientLength = 40;

std::u16string ToUtf16(std::string_view text)
{
    std::u16string out;
    utf8::utf8to16(text.begin(), text.end(), std::back_inserter(out));
    return out;
}

std::string ToUtf8(std::u16string const& text)
{
    std::string out;
    utf8::utf16to8(text.begin(), text.end(), std::back_inserter(out));
    return out;
}

// Characters, as the server counts them elsewhere: a pair of surrogates is one
std::size_t CountCharacters(std::u16string const& text)
{
    return std::ranges::count_if(text, [](char16_t unit) { return unit < 0xDC00 || unit > 0xDFFF; });
}

// CodeMirror's ChangeSet.toJSON() on text: a number keeps that many UTF-16 units, [n] deletes n,
// [n, "line", "line"...] replaces n by the lines joined by \n. False, text unchanged, when it isn't
// one or doesn't cover the text exactly
bool ApplyChanges(std::u16string& text, boost::json::value const& changes)
{
    boost::json::array const* parts = changes.if_array();
    if (!parts)
        return false;

    std::u16string out;
    out.reserve(text.size());
    std::size_t at = 0;
    try
    {
        for (boost::json::value const& part : *parts)
        {
            if (part.is_int64() || part.is_uint64())
            {
                int64 length = part.to_number<int64>();
                if (length < 0 || at + std::size_t(length) > text.size())
                    return false;
                out.append(text, at, std::size_t(length));
                at += std::size_t(length);
                continue;
            }

            boost::json::array const* replace = part.if_array();
            if (!replace || replace->empty() || !(replace->front().is_int64() || replace->front().is_uint64()))
                return false;
            int64 length = replace->front().to_number<int64>();
            if (length < 0 || at + std::size_t(length) > text.size())
                return false;
            at += std::size_t(length);
            for (std::size_t line = 1; line < replace->size(); ++line)
            {
                boost::json::string const* inserted = (*replace)[line].if_string();
                if (!inserted)
                    return false;
                if (line > 1)
                    out += u'\n';
                out += ToUtf16(*inserted);
            }
        }
    }
    catch (std::exception const&)
    {
        // Not UTF-8, or a number that isn't one
        return false;
    }

    if (at != text.size())
        return false;
    text = std::move(out);
    return true;
}

BlogDatabaseStatements SaveStatement(std::string_view field)
{
    if (field == "title")
        return BLOG_UPS_POST_DRAFT_TITLE;
    if (field == "summary")
        return BLOG_UPS_POST_DRAFT_SUMMARY;
    return BLOG_UPS_POST_DRAFT_BODY;
}
}

Optional<DocKey> DocKey::Parse(std::string_view name)
{
    // post:<id>:<lang>:<field>
    if (!name.starts_with("post:"))
        return {};
    name.remove_prefix(5);
    std::size_t colon = name.find(':');
    if (colon == std::string_view::npos)
        return {};
    Optional<uint32> id = Rooftree::StringTo<uint32>(name.substr(0, colon));
    name.remove_prefix(colon + 1);
    colon = name.find(':');
    if (!id || !*id || colon == std::string_view::npos)
        return {};
    std::string_view lang = name.substr(0, colon);
    std::string_view field = name.substr(colon + 1);
    if (!IsValidLang(lang) || (field != "title" && field != "summary" && field != "body"))
        return {};
    return DocKey{ .PostId = *id, .Lang = std::string(lang), .Field = std::string(field) };
}

std::string DocKey::Name() const
{
    return "post:" + std::to_string(PostId) + ":" + Lang + ":" + Field;
}

std::size_t DocKey::MaxLength() const
{
    if (Field == "title")
        return MaxTitleLength;
    if (Field == "summary")
        return MaxSummaryLength;
    return MaxBodyLength;
}

boost::json::object Collab::PeerJson(Peer const& peer)
{
    boost::json::object user{ { "id", peer.Info.UserId }, { "username", peer.Info.Username }, { "displayName", peer.Info.DisplayName },
        { "avatar", peer.Info.Avatar.empty() ? boost::json::value(nullptr) : boost::json::value("/uploads/" + peer.Info.Avatar) } };
    return { { "client", peer.Info.Client }, { "user", std::move(user) }, { "selection", peer.Selection } };
}

void Collab::Send(Doc const& doc, std::string_view op, boost::json::object body, Rooftree::Server::WorldWebSocket const* except) const
{
    body["op"] = op;
    for (auto const& [key, peer] : doc.Peers)
        if (key != except)
            if (std::shared_ptr<Rooftree::Server::WorldWebSocket> socket = peer.Socket.lock())
                socket->SendJson("doc", body);
}

void Collab::Open(DocKey const& key, std::shared_ptr<Rooftree::Server::WorldWebSocket> const& socket, DocPeer info, Optional<uint32> since,
    Optional<DocText> loaded)
{
    std::string name = key.Name();
    if (info.Client.empty() || info.Client.size() > MaxClientLength)
        return socket->SendJson("doc", { { "op", "error" }, { "doc", name }, { "code", "invalid" } });

    auto itr = _docs.find(key);
    if (itr == _docs.end())
    {
        // Loaded by the page's session: the first one to come back makes it
        if (!loaded)
            return socket->SendJson("doc", { { "op", "error" }, { "doc", name }, { "code", "not_found" } });
        Doc doc;
        try
        {
            doc.Text = ToUtf16(loaded->Text);
        }
        catch (std::exception const&)
        {
            RT_LOG_ERROR("blog.collab", "{} isn't UTF-8 in the database", name);
        }
        doc.Version = loaded->Version;
        doc.RecentBase = doc.Version;
        doc.SavedVersion = doc.Version;
        itr = _docs.emplace(key, std::move(doc)).first;
    }

    Doc& doc = itr->second;
    Peer peer{ .Socket = socket, .Info = std::move(info) };
    boost::json::object joined = PeerJson(peer);
    joined["doc"] = name;
    Send(doc, "peer", joined, socket.get());

    boost::json::array peers;
    for (auto const& [other, present] : doc.Peers)
        if (other != socket.get())
            peers.push_back(PeerJson(present));
    doc.Peers[socket.get()] = std::move(peer);

    // A page that was away: the changes since its version, while they are kept
    if (since && *since >= doc.RecentBase && *since <= doc.Version)
    {
        boost::json::array updates;
        for (std::size_t i = *since - doc.RecentBase; i < doc.Recent.size(); ++i)
            updates.push_back({ { "changes", doc.Recent[i].Changes }, { "client", doc.Recent[i].Client } });
        socket->SendJson("doc", { { "op", "updates" }, { "doc", name }, { "version", *since }, { "updates", std::move(updates) },
            { "peers", std::move(peers) } });
        return;
    }
    socket->SendJson("doc", { { "op", "state" }, { "doc", name }, { "version", doc.Version }, { "text", ToUtf8(doc.Text) },
        { "peers", std::move(peers) } });
}

void Collab::Push(DocKey const& key, std::shared_ptr<Rooftree::Server::WorldWebSocket> const& socket, uint32 version,
    boost::json::value const& updates)
{
    std::string name = key.Name();
    auto itr = _docs.find(key);
    if (itr == _docs.end() || !itr->second.Peers.contains(socket.get()))
        return socket->SendJson("doc", { { "op", "error" }, { "doc", name }, { "code", "not_open" } });

    Doc& doc = itr->second;
    // Made on a version it didn't have yet: the newer changes are on their way to it
    if (version != doc.Version)
        return socket->SendJson("doc", { { "op", "rejected" }, { "doc", name } });

    boost::json::array const* list = updates.if_array();
    if (!list || list->empty() || list->size() > MaxChangesPerPush)
        return socket->SendJson("doc", { { "op", "error" }, { "doc", name }, { "code", "invalid" } });

    // All of them or none
    std::u16string text = doc.Text;
    std::vector<Change> taken;
    for (boost::json::value const& update : *list)
    {
        boost::json::object const* fields = update.if_object();
        boost::json::value const* changes = fields ? fields->if_contains("changes") : nullptr;
        boost::json::value const* client = fields ? fields->if_contains("client") : nullptr;
        if (!changes || !client || !client->is_string() || client->as_string().size() > MaxClientLength || !ApplyChanges(text, *changes))
            return socket->SendJson("doc", { { "op", "error" }, { "doc", name }, { "code", "invalid" } });
        taken.push_back({ .Changes = *changes, .Client = std::string(client->as_string()) });
    }
    if (CountCharacters(text) > key.MaxLength())
        return socket->SendJson("doc", { { "op", "error" }, { "doc", name }, { "code", "too_long" } });

    boost::json::array sent;
    for (Change const& change : taken)
        sent.push_back({ { "changes", change.Changes }, { "client", change.Client } });
    Send(doc, "updates", { { "doc", name }, { "version", doc.Version }, { "updates", std::move(sent) } });

    doc.Text = std::move(text);
    doc.Version += uint32(taken.size());
    for (Change& change : taken)
        doc.Recent.push_back(std::move(change));
    while (doc.Recent.size() > KeptChanges)
    {
        doc.Recent.pop_front();
        ++doc.RecentBase;
    }
}

void Collab::Cursor(DocKey const& key, std::shared_ptr<Rooftree::Server::WorldWebSocket> const& socket, boost::json::value selection)
{
    auto itr = _docs.find(key);
    if (itr == _docs.end())
        return;
    auto peer = itr->second.Peers.find(socket.get());
    if (peer == itr->second.Peers.end())
        return;

    peer->second.Selection = std::move(selection);
    boost::json::object body = PeerJson(peer->second);
    body["doc"] = key.Name();
    Send(itr->second, "peer", std::move(body), socket.get());
}

void Collab::Close(DocKey const& key, Rooftree::Server::WorldWebSocket const* socket)
{
    auto itr = _docs.find(key);
    if (itr != _docs.end())
        RemovePeer(key, itr->second, socket);
}

void Collab::RemovePeer(DocKey const& key, Doc& doc, Rooftree::Server::WorldWebSocket const* socket)
{
    auto peer = doc.Peers.find(socket);
    if (peer == doc.Peers.end())
        return;

    std::string client = peer->second.Info.Client;
    doc.Peers.erase(peer);
    Send(doc, "left", { { "doc", key.Name() }, { "client", client } });
    if (doc.Peers.empty())
        doc.EmptySince = TimePoint::clock::now();
}

void Collab::Revoke(uint32 postId, uint32 userId)
{
    for (auto& [key, doc] : _docs)
    {
        if (key.PostId != postId)
            continue;
        std::vector<Rooftree::Server::WorldWebSocket const*> removed;
        for (auto const& [socket, peer] : doc.Peers)
            if (peer.Info.UserId == userId)
                removed.push_back(socket);
        for (Rooftree::Server::WorldWebSocket const* socket : removed)
        {
            if (std::shared_ptr<Rooftree::Server::WorldWebSocket> open = doc.Peers[socket].Socket.lock())
                open->SendJson("doc", { { "op", "closed" }, { "doc", key.Name() }, { "reason", "removed" } });
            RemovePeer(key, doc, socket);
        }
    }
}

void Collab::Forget(uint32 postId, std::string_view reason)
{
    for (auto itr = _docs.begin(); itr != _docs.end();)
    {
        if (itr->first.PostId != postId)
        {
            ++itr;
            continue;
        }
        // What it had and wasn't written is gone with it: the post's text is another now
        Send(itr->second, "closed", { { "doc", itr->first.Name() }, { "reason", reason } });
        itr = _docs.erase(itr);
    }
}

std::vector<DocSnapshot> Collab::GetSnapshots(uint32 postId) const
{
    std::vector<DocSnapshot> snapshots;
    for (auto const& [key, doc] : _docs)
        // Written or not: a write sent a moment ago may still be on its way
        if (key.PostId == postId)
            snapshots.push_back({ .Key = key, .Text = ToUtf8(doc.Text), .Version = doc.Version });
    return snapshots;
}

void Collab::Save(DocKey const& key, Doc& doc, bool blocking)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(SaveStatement(key.Field));
    stmt->setUInt32(0, key.PostId);
    stmt->setString(1, key.Lang);
    stmt->setString(2, ToUtf8(doc.Text));
    stmt->setUInt32(3, doc.Version);
    if (blocking)
        BlogDatabase.DirectExecute(stmt);
    else
        BlogDatabase.Execute(stmt);
    doc.SavedVersion = doc.Version;
}

void Collab::Update(uint32 diff)
{
    _sinceSave += Milliseconds(diff);
    if (_sinceSave < SaveInterval)
        return;
    _sinceSave = 0ms;

    TimePoint now = TimePoint::clock::now();
    for (auto itr = _docs.begin(); itr != _docs.end();)
    {
        auto& [key, doc] = *itr;
        std::vector<Rooftree::Server::WorldWebSocket const*> gone;
        for (auto const& [socket, peer] : doc.Peers)
            if (peer.Socket.expired())
                gone.push_back(socket);
        for (Rooftree::Server::WorldWebSocket const* socket : gone)
            RemovePeer(key, doc, socket);

        if (doc.Version != doc.SavedVersion)
            Save(key, doc, false);
        if (doc.Peers.empty() && now - doc.EmptySince > ForgetAfter)
            itr = _docs.erase(itr);
        else
            ++itr;
    }
}

void Collab::SaveAll()
{
    for (auto& [key, doc] : _docs)
        if (doc.Version != doc.SavedVersion)
            Save(key, doc, true);
}
}
