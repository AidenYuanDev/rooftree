/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Any route of the API from the console (ConsoleCommands.h), for what the console's own commands
 * don't do: what a reader does (logging in, commenting, a drive's uploads and links, subscribing). The
 * admin's pages have commands of their own (ConsoleAdmin.h), which run in the server without HTTP.
 * "api get|post|put|patch|delete <path>" sends a real request to the server's own plain HTTP port on
 * 127.0.0.1, logged in as the admin (or --as another account), through the same handler, checks,
 * AntiDOS and log as the web's.
 *
 *   api routes [text]                              the routes, those whose path has text
 *   api <method> <path> [options] [body]
 *     --as <account>                               act as this account instead of the admin
 *     -H "Name: value"                             one more header (Content-Type of an @file)
 *     --out <file>                                 the response's body into file instead of printed
 *     --form                                       the items as multipart/form-data, key@file a file
 *   body, one of:
 *     {"a": 1} or [...]                            JSON as it is
 *     @<file>                                      the file's bytes
 *     key=text key:=json key=@file key:=@file      a JSON object (httpie's items): text, JSON, the
 *                                                  file as a string, the file as JSON; a.b=x nests
 *
 * The console acts as the account with a login session of its own, made for the one request and
 * deleted after it (user agent "blog console"): no password or TOTP, the console may do anything.
 * Files are read by the server, so their paths are the server's (in the container with Docker).
 */

#ifndef BLOG_CONSOLE_API_H
#define BLOG_CONSOLE_API_H

#include "ConsoleCommand.h"

namespace Rooftree::Server
{
class WorldHttpService;
}

namespace Blog
{
// Main, before the console starts: the plain service (its routes) and where it listens
void SetConsoleApi(Rooftree::Server::WorldHttpService const* service, std::string bindIp, uint16 port);

// The "api" command of GetConsoleCommands()
Rooftree::Server::Command GetConsoleApiCommand();
}

#endif // BLOG_CONSOLE_API_H
