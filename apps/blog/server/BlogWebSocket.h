/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The browsers' WebSocket (/ws on the site's own port, plain or TLS), for what changes while a page is
 * open: how many read it (Presence.h) and what was just published. The HTTP socket of the handshake
 * runs it (WorldHttpService::WebSocketRoute), and its messages go to the same SessionMgr as the API,
 * with the same session key (the client address), so an address's requests and messages are one
 * session.
 *
 * The handshake checks the Origin and keeps the hash of the login cookie it came with: the pages of
 * a logged in reader tell the admin that the reader is online (Presence.h). Nothing else on it needs
 * to know who reads. A login or logout on the page opens a new connection; a logout or a password
 * change sent as a packet changes the login of this one (SetLoginHash).
 *
 *   browser -> {"type": "page", "path": "/zh/posts/hello", "watch": ["post:12"]}   on open, on navigation,
 *                                                                               on a change of watch, every 25 s
 *   server  -> {"type": "presence", "page": 2, "site": 9}          when the counts change
 *   server  -> {"type": "published", "kind": "post", "path": "/posts/hello", "title": "..."}
 *   server  -> {"type": "changed", "target": "post:12", "what": "comments"}   (Presence.h)
 *
 * Binary messages are the API's packets (ApiPackets.h). A connection with a login may send messages
 * as big as an HTTP request's body, the others 4 KiB.
 */

#ifndef BLOG_WEB_SOCKET_H
#define BLOG_WEB_SOCKET_H

#include "WorldWebSocket.h"
#include <string>
#include <vector>

namespace Rooftree::Server
{
class WebSocketRouter;
class WorldHttpService;
}

namespace Blog
{
// Every connection of /ws is one
class BlogWebSocket final : public Rooftree::Server::WorldWebSocket
{
public:
    BlogWebSocket(Rooftree::Server::WebSocketRouter const& router, boost::asio::ip::address remoteAddress, std::string sessionKey,
        std::string loginHash, std::string userAgent) : WorldWebSocket(router, std::move(remoteAddress), std::move(sessionKey)),
        _loginHash(std::move(loginHash)), _userAgent(std::move(userAgent)) { }

    // SHA-256 of the handshake's session cookie, empty without one. Main thread: the packets' operations
    // call as it; a logout ends it, a password change replaces it (ApiPackets.cpp)
    std::string const& GetLoginHash() const { return _loginHash; }
    void SetLoginHash(std::string loginHash) { _loginHash = std::move(loginHash); }
    // The handshake's User-Agent, what its packets' operations see
    std::string const& GetUserAgent() const { return _userAgent; }

private:
    std::string _loginHash;
    std::string _userAgent;
};

// /ws on service; router, allowedOrigins and the session cookie's name as they are at startup, router
// outlives the service
void RegisterWebSocketRoute(Rooftree::Server::WorldHttpService& service, Rooftree::Server::WebSocketRouter const& router,
    std::vector<std::string> allowedOrigins, std::string cookieName);

// The message handlers and the API's packets, on the router
void RegisterWebSocketRoutes(Rooftree::Server::WebSocketRouter& router);
}

#endif // BLOG_WEB_SOCKET_H
