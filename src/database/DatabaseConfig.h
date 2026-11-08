/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A database's settings as a YAML mapping:
 *
 *   Database:
 *     Login:
 *       Host: 127.0.0.1
 *       Port: 5432
 *       User: rooftree
 *       Password: rooftree
 *       Name: auth
 *       WorkerThreads: 1
 *       SynchThreads: 1
 *
 *   std::optional<DatabaseConfig> login = config->Get<DatabaseConfig>("Database.Login");
 *   LoginDatabase.SetConnectionInfo(login->Connection, login->WorkerThreads, login->SynchThreads);
 *
 * Get returns nullopt when Host, User or Name is missing or empty, or a value does not convert.
 */

#ifndef ROOFTREE_DATABASECONFIG_H
#define ROOFTREE_DATABASECONFIG_H

#include "DatabaseConnection.h"
#include <yaml-cpp/yaml.h>
#include <string>

struct DatabaseConfig
{
    DatabaseConnectionInfo Connection;
    uint8 WorkerThreads = 1;    // asynchronous connections
    uint8 SynchThreads = 1;     // synchronous connections
};

namespace Rooftree::Impl
{
// A field that may be left out (or left empty), but has to convert when it is there: Port: abc fails
// the whole mapping instead of silently becoming 5432
template <typename T>
T GetDatabaseConfigField(YAML::Node const& node, char const* key, T def)
{
    YAML::Node value = node[key];
    return value && !value.IsNull() ? value.as<T>() : def;
}
}

// Host, Port, User, Password, Name, and optionally:
//   Socket: connect through the Unix socket in this directory instead of Host and Port
//   Ssl:    true = required, false = disabled, left out = libpq's default, TLS if the server offers it
template <>
struct YAML::convert<DatabaseConnectionInfo>
{
    static bool decode(Node const& node, DatabaseConnectionInfo& info)
    {
        using Rooftree::Impl::GetDatabaseConfigField;
        if (!node.IsMap())
            return false;

        info.host = GetDatabaseConfigField<std::string>(node, "Host", "");
        info.user = GetDatabaseConfigField<std::string>(node, "User", "");
        info.password = GetDatabaseConfigField<std::string>(node, "Password", "");
        info.database = GetDatabaseConfigField<std::string>(node, "Name", "");
        if (info.host.empty() || info.user.empty() || info.database.empty())
            return false;

        // DatabaseConnection's convention: host "." means port_or_socket is a socket's directory
        if (std::string socket = GetDatabaseConfigField<std::string>(node, "Socket", ""); !socket.empty())
        {
            info.host = ".";
            info.port_or_socket = std::move(socket);
        }
        else
            info.port_or_socket = std::to_string(GetDatabaseConfigField<uint16>(node, "Port", 5432));

        // DatabaseConnection requires TLS for "ssl", refuses it for "disabled", takes what is offered otherwise
        YAML::Node ssl = node["Ssl"];
        info.ssl = !ssl || ssl.IsNull() ? "" : ssl.as<bool>() ? "ssl" : "disabled";
        return true;
    }
};

template <>
struct YAML::convert<DatabaseConfig>
{
    static bool decode(Node const& node, DatabaseConfig& config)
    {
        using Rooftree::Impl::GetDatabaseConfigField;
        if (!YAML::convert<DatabaseConnectionInfo>::decode(node, config.Connection))
            return false;

        config.WorkerThreads = GetDatabaseConfigField<uint8>(node, "WorkerThreads", 1);
        config.SynchThreads = GetDatabaseConfigField<uint8>(node, "SynchThreads", 1);
        return true;
    }
};

#endif // ROOFTREE_DATABASECONFIG_H
