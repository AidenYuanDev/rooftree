/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "AdminCli.h"
#include "Argon2Hash.h"
#include "BlogDatabase.h"
#include "BlogSettings.h"
#include "BlogTypes.h"
#include "CryptoRandom.h"
#include "Field.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "TOTP.h"
#include "TotpSetup.h"
#include "Validation.h"
#include <print>
#include <iostream>
#include <termios.h>
#include <unistd.h>

namespace Blog
{
namespace
{
std::string ReadPassword(char const* prompt)
{
    std::cerr << prompt << std::flush;
    termios old{};
    bool terminal = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (terminal)
    {
        termios silent = old;
        silent.c_lflag &= ~tcflag_t(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &silent);
    }

    std::string password;
    std::getline(std::cin, password);
    if (terminal)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        std::cerr << '\n';
    }

    return password;
}
}

int CreateAdmin(CreateAdminArguments const& arguments, BlogSettings const& settings)
{
    std::string displayName = arguments.DisplayName.empty() ? arguments.Username : arguments.DisplayName;
    if (!IsValidUsername(arguments.Username) || !IsValidEmail(arguments.Email)
        || displayName.empty() || CountCharacters(displayName) > 40)
    {
        std::print(stderr, "--username needs 3 to 20 letters, digits or _, --email an address, --display-name at most 40 characters\n");
        return 1;
    }

    PreparedQueryResult admins = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_EXISTS));
    if (admins && admins->Fetch()[0].GetInt64() > 0)
    {
        std::print(stderr, "There is an admin already, the blog has a single author. Its password and two-step login are set on the\n"
            "server's console: account set password, account set 2fa\n");
        return 1;
    }

    std::string password;
    if (char const* fromEnvironment = std::getenv("BLOG_ADMIN_PASSWORD"))
        password = fromEnvironment;
    else
    {
        password = ReadPassword("Password: ");
        if (ReadPassword("Again: ") != password)
        {
            std::print(stderr, "The passwords differ\n");
            return 1;
        }
    }

    if (CountCharacters(password) < 12 || password.size() > 128)
    {
        std::print(stderr, "The admin password needs 12 to 128 characters\n");
        return 1;
    }
    Optional<std::string> hash = Rooftree::Crypto::Argon2::Hash(password, settings.Argon2Iterations, settings.Argon2MemoryCost);
    if (!hash)
    {
        std::print(stderr, "Hashing the password failed\n");
        return 1;
    }

    std::vector<uint8> secret;
    if (arguments.Totp)
    {
        secret.resize(Rooftree::Crypto::TOTP::SecretSize);
        Rooftree::Crypto::GetRandomBytes(secret);
    }

    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_USER);
    stmt->setString(0, arguments.Username);
    stmt->setString(1, displayName);
    stmt->setString(2, arguments.Email);
    stmt->setString(3, *hash);
    stmt->setString(4, RoleAdmin);
    // The site's first language, as for readers who register
    stmt->setString(5, std::string_view("zh"));
    stmt->setDate(6, SystemTimePoint::clock::now());
    if (arguments.Totp)
        stmt->setBinary(7, std::span<uint8 const>(secret));
    else
        stmt->setNull(7);
    BlogDatabase.DirectExecute(stmt);

    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_ID_AND_HASH_BY_NAME);
    stmt->setString(0, arguments.Username);
    PreparedQueryResult created = BlogDatabase.Query(stmt);
    if (!created || created->Fetch()[1].GetString() != *hash)
    {
        std::print(stderr, "The user name or the address is taken\n");
        return 1;
    }

    std::print("Created admin {} ({}).\n", arguments.Username, created->Fetch()[0].GetUInt32());
    if (arguments.Totp)
        std::print("\n{}\n", FormatTotpSetup(settings.TotpIssuer, arguments.Username, secret, isatty(STDOUT_FILENO)));
    return 0;
}
}
