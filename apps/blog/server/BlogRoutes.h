/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#ifndef BLOG_ROUTES_H
#define BLOG_ROUTES_H

#include <string>

namespace Rooftree::Server
{
class StaticSiteHolder;
class WorldHttpService;
}

namespace Blog
{
// Every /api route, each with the message type its AntiDOS limit is configured by
void RegisterRoutes(Rooftree::Server::WorldHttpService& service);

// The site, the uploads, the ACME challenges (empty acmeDirectory: none) and the redirects, answered on
// the network threads (StaticHandlers.cpp). site outlives the service
void RegisterStaticRoutes(Rooftree::Server::WorldHttpService& service, Rooftree::Server::StaticSiteHolder const& site,
    std::string uploadDirectory, std::string acmeDirectory);
}

#endif // BLOG_ROUTES_H
