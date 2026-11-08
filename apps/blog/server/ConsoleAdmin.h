/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The admin's pages as console commands: each runs the same ApiOp as its route (ApiCall.h), in
 * the server, as the admin, and prints the answer as text. What only a reader does (logging in,
 * commenting, a drive's uploads...) is for "api" (ConsoleApi.h).
 *
 *   post list [draft|published] [page]             post show <post>, post delete <post>
 *   post create <slug> <zh|en> <title> <file> [published]      a draft unless published
 *   post edit <post> slug|status|lang|cover|comments|tags|publish-at <value>
 *   post translation set <post> <zh|en> <title> <file> [summary], post translation delete <post> <zh|en>
 *   post publish|unpublish|decline <post>, post list review           what writers asked to be published
 *   post revisions <post> [version], post save-version <post> [note], post restore <post> <version>
 *   post editors <post>, post editor add <post> <account> [publish,settings,editors|none],
 *   post editor remove <post> <account>, post owner <post> <account>
 *   tag list, tag add|edit <slug> <name zh> <name en>, tag delete <slug>
 *   note list|show|create|edit|publish|unpublish|delete, note translation set|delete   like post's, by id
 *   thinking list [page], thinking add <zh|en> <text>, thinking edit <id> <zh|en> <text>,
 *   thinking publish|unpublish|delete <id>
 *   comment list [visible|hidden|deleted] [before <id>], comment hide|show <id>
 *   friend list [pending|approved|rejected], friend approve <id>, friend reject <id> [reason],
 *   friend add <name> <url> [description], friend edit <id> name|url|avatar|description|sort <value>,
 *   friend delete <id>
 *   project list, project add <name> [url], project edit <id> name|url|repo|icon|zh|en|sort <value>,
 *   project delete <id>
 *   upload <file>                                  an image for posts, its address
 *   account show|sessions|kick [session]|reset-mail <account>, account create <name> <email> [display name],
 *   account role <account> user|moderator, account deny|allow <account> comment|react|avatar|friend,
 *   account drive <account> on|off [size like 5G|default] [trash days|default], account write <account> on|off
 *   online, stats [days], status
 *
 * <post> is a slug or an id, <account> a user name or an address, <file> a path on the server.
 */

#ifndef BLOG_CONSOLE_ADMIN_H
#define BLOG_CONSOLE_ADMIN_H

#include "ConsoleCommand.h"

namespace Blog
{
// The commands above, for GetConsoleCommands(); account's are added to its own
std::vector<Rooftree::Server::Command> GetConsoleAdminCommands();
std::vector<Rooftree::Server::Command> GetConsoleAccountAdminCommands();

// Worker: a file the console names, false with error set when it can't be read (or is too large)
bool ReadConsoleFile(std::string const& path, std::string& data, std::string& error);
}

#endif // BLOG_CONSOLE_ADMIN_H
