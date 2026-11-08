/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A post's editors writing together, in real time: each field of a language of a post (its title,
 * summary or Markdown) is a document, "post:12:zh:body", that the editors' pages open over their
 * WebSocket (BlogWebSocket.h). The main thread holds every open document and is the order of its
 * changes: a page sends the changes it made on top of the version it has (CodeMirror's ChangeSet,
 * @codemirror/collab), the document takes them only if that version is its own, and sends them to
 * every page that has it open, the sender's too, which so learns they were taken. Changes on an
 * older version are refused; that page gets the newer ones meanwhile, puts its own on top and sends
 * them again. Where each editor's cursor is goes to the others too.
 *
 * What the editors write is the post's working copy (post_draft): readers see the published text
 * (post_translation) until someone with the publish right publishes it (PostHandlers.cpp). A changed
 * document is written once a second, with its version: a write that arrives late never replaces a
 * newer one. One no page has open is forgotten a minute after.
 *
 *   page   -> {"type": "doc", "op": "open", "doc": "post:12:zh:body", "client": "k3x...", "since": 41}
 *   server -> {"type": "doc", "op": "state", "doc", "version": 57, "text": "...", "peers": [...]}
 *             or, since a version it still has, "updates" from there on
 *   page   -> {"type": "doc", "op": "push", "doc", "version": 57, "updates": [{"changes": [...], "client"}]}
 *   server -> {"type": "doc", "op": "updates", "doc", "version": 57, "updates": [...]} to all of them,
 *             or {"op": "rejected"} to the sender
 *   page   -> {"type": "doc", "op": "cursor", "doc", "selection": {...}}, {"op": "close", "doc"}
 *   server -> {"op": "peer", "doc", "client", "user", "selection"}, {"op": "left", "doc", "client"},
 *             {"op": "closed", "doc", "reason": "removed"|"replaced"|"deleted"}, {"op": "error", "doc", "code"}
 *
 * Main thread only, like everything in BlogWorld.
 */

#ifndef BLOG_COLLAB_H
#define BLOG_COLLAB_H

#include "Define.h"
#include "Duration.h"
#include "Optional.h"
#include <boost/json/value.hpp>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rooftree::Server
{
class WorldWebSocket;
}

namespace Blog
{
struct DocKey
{
    uint32 PostId = 0;
    std::string Lang;       // zh, en
    std::string Field;      // title, summary, body

    // "post:12:zh:body"
    static Optional<DocKey> Parse(std::string_view name);
    std::string Name() const;
    // In characters
    std::size_t MaxLength() const;

    auto operator<=>(DocKey const&) const = default;
};

// One editor's page with a document open
struct DocPeer
{
    std::string Client;     // the page's own id, of its changes too
    uint32 UserId = 0;
    std::string Username, DisplayName, Avatar;
};

// A document's text and version as the database has it
struct DocText
{
    std::string Text;
    uint32 Version = 0;
};

// What publishing writes first: the newest text of an open document
struct DocSnapshot
{
    DocKey Key;
    std::string Text;
    uint32 Version = 0;
};

class Collab
{
public:
    // Open: the page may edit the post (its session checked), loaded the document's text if it isn't
    // open; since: the version the page has, from which it gets the changes if they are still here
    bool IsLoaded(DocKey const& key) const { return _docs.contains(key); }
    void Open(DocKey const& key, std::shared_ptr<Rooftree::Server::WorldWebSocket> const& socket, DocPeer peer, Optional<uint32> since,
        Optional<DocText> loaded);
    void Push(DocKey const& key, std::shared_ptr<Rooftree::Server::WorldWebSocket> const& socket, uint32 version, boost::json::value const& updates);
    void Cursor(DocKey const& key, std::shared_ptr<Rooftree::Server::WorldWebSocket> const& socket, boost::json::value selection);
    void Close(DocKey const& key, Rooftree::Server::WorldWebSocket const* socket);

    // An editor removed from the post: its pages lose its documents
    void Revoke(uint32 postId, uint32 userId);
    // The post's text replaced from elsewhere (an update with its text, a language deleted), or the
    // post deleted: its documents are closed, the pages open them again
    void Forget(uint32 postId, std::string_view reason);
    // The newest text of the post's open documents, for publishing (which writes them first)
    std::vector<DocSnapshot> GetSnapshots(uint32 postId) const;

    // Pages gone, changes written, documents no page has open forgotten
    void Update(uint32 diff);
    // Shutdown: every change written (blocking)
    void SaveAll();

    std::size_t GetOpenCount() const { return _docs.size(); }

private:
    struct Peer
    {
        std::weak_ptr<Rooftree::Server::WorldWebSocket> Socket;
        DocPeer Info;
        boost::json::value Selection;
    };

    struct Change
    {
        boost::json::value Changes;
        std::string Client;
    };

    struct Doc
    {
        std::u16string Text;
        uint32 Version = 0;
        // The last changes, Recent[0] made version RecentBase into RecentBase + 1
        std::deque<Change> Recent;
        uint32 RecentBase = 0;
        uint32 SavedVersion = 0;
        std::unordered_map<Rooftree::Server::WorldWebSocket const*, Peer> Peers;
        TimePoint EmptySince;
    };

    void Send(Doc const& doc, std::string_view op, boost::json::object body, Rooftree::Server::WorldWebSocket const* except = nullptr) const;
    void RemovePeer(DocKey const& key, Doc& doc, Rooftree::Server::WorldWebSocket const* socket);
    void Save(DocKey const& key, Doc& doc, bool blocking);
    static boost::json::object PeerJson(Peer const& peer);

    std::map<DocKey, Doc> _docs;
    Milliseconds _sinceSave = 0ms;
};
}

#endif // BLOG_COLLAB_H
