/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * What the accounts' own requests (AuthHandlers.cpp) and the admin's management of them
 * (UserAdminHandlers.cpp) share: hashing passwords, mailed links, sessions. Worker* ones block on the
 * database, for OnWorker.
 */

#ifndef BLOG_ACCOUNTS_H
#define BLOG_ACCOUNTS_H

#include "BlogTypes.h"
#include "Mailer.h"

namespace Blog
{
struct HashSettings
{
    uint32 Iterations;
    uint32 MemoryCost;
};

// Main thread: Blog.Argon2's, for a worker to hash with
HashSettings GetHashSettings();
// Argon2id with a new salt; none when hashing failed
Optional<std::string> HashPassword(std::string const& password, HashSettings settings);

// The mails, in the user's language; main thread (the site's name)
Rooftree::Server::MailMessage VerificationMail(UserRow const& user, std::string_view token);
Rooftree::Server::MailMessage ResetMail(UserRow const& user, std::string_view token);
// An account the admin made without a password: a reset link (lifetime long) to choose one
Rooftree::Server::MailMessage InviteMail(UserRow const& user, std::string_view token, Seconds lifetime);

Optional<UserRow> FindUserById(uint32 id);
// A new mailed token for user, replacing the unused ones of its kind
std::string WorkerCreateEmailToken(uint32 userId, std::string_view purpose, Seconds lifetime);
void WorkerDeleteUserSessions(uint32 userId);

struct NewAuthSession
{
    std::string Token;          // the cookie
    std::string Hash;           // its row's id
    std::string CsrfToken;
    SystemTimePoint ExpiresAt;
};
// Inserts a login session for the user (a login, or the console's api command acting as it)
NewAuthSession WorkerCreateAuthSession(uint32 userId, Seconds lifetime, std::string const& ip, std::string const& userAgent);
bool WorkerSetPassword(uint32 userId, std::string const& password, HashSettings hashSettings);
}

#endif // BLOG_ACCOUNTS_H
