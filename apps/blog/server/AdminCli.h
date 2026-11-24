/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * blog --create-admin: the author's account, created from the command line before the server first
 * runs. Its password comes from BLOG_ADMIN_PASSWORD or is asked for on the terminal; with totp, the
 * account has two-step login and its secret is printed.
 */

#ifndef BLOG_ADMIN_CLI_H
#define BLOG_ADMIN_CLI_H

#include <string>

namespace Blog {
struct BlogSettings;

struct CreateAdminArguments {
  std::string Username;
  std::string Email;
  std::string DisplayName;  // Username when empty
  bool Totp = false;
};

// With the database open. The process' exit code: 0 created, 1 refused (printed on stderr)
int CreateAdmin(CreateAdminArguments const& arguments, BlogSettings const& settings);
}  // namespace Blog

#endif  // BLOG_ADMIN_CLI_H
