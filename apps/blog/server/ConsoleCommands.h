/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The blog's console commands (Console.h, ConsoleSocket.h):
 *
 *   account list                                   the newest accounts
 *   lookup account <text>                          by part of the name, address or display name
 *   account set password <account>                 asks twice, not echoed (or give it twice after the name)
 *   account set 2fa <account> [off]                a new TOTP secret, as a QR code; off: none (not for the admin)
 *   account set username|email|name <account> <value>
 *   account delete <account>                       not while it has posts or uploads
 *   ban account <account> <time|perm> [reason]     time like 30m, 1d12h
 *   ban ip <address> <time|perm> [reason]
 *   unban account <account>, unban ip <address>
 *   banlist account, banlist ip
 *   site show, site export                         the site's profile, export as one line of JSON
 *   site import <json>                             the whole profile, checked like the admin's page
 *   site set <field> [zh|en] <text>                one field; texts of more lines and links: import
 *   site review on|off                             writers' publishing asks the admin first
 *   server info, server reload (SIGHUP)
 *   server shutdown|restart <time>|cancel          exit code 0, restart 2: the supervisor decides
 *   post, tag, note, thinking, comment, friend, project, upload, online, stats, status, and more of
 *   account (show, create, role, deny, drive, kick...)   the admin's pages (ConsoleAdmin.h)
 *   api routes, api get|post|put|patch|delete <path> ...
 *                                                  any route of the API, for what a reader does (ConsoleApi.h)
 *
 * <account> is a user name or an email address. A new password or TOTP secret ends the account's
 * sessions.
 */

#ifndef BLOG_CONSOLE_COMMANDS_H
#define BLOG_CONSOLE_COMMANDS_H

#include "ConsoleCommand.h"

namespace Rooftree::Server
{
class UpdateLoop;
}

namespace Blog
{
Rooftree::Server::CommandTable const& GetConsoleCommands();

// The loop the server commands report on and stop, set by main before the console starts
void SetConsoleUpdateLoop(Rooftree::Server::UpdateLoop* loop);
}

#endif // BLOG_CONSOLE_COMMANDS_H
