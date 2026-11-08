/*
 * The database tests and benchmarks may use: Database.Test of build/bin/rooftree_test.yaml (created from
 * tests/rooftree_test.yaml.dist), or of the file named by ROOFTREE_TEST_CONFIG. Shared by tests/ and benchmarks/.
 *
 * Parsed into a Config of its own, sConfigMgr is left alone: tests load their own configs into it.
 */

#ifndef ROOFTREE_TESTDATABASECONFIG_H
#define ROOFTREE_TESTDATABASECONFIG_H

#include "Config.h"
#include "DatabaseConfig.h"
#include <cstdlib>
#include <optional>
#include <string>

// Set by tests/ and benchmarks/CMakeLists.txt to build/bin/rooftree_test.yaml
#ifndef ROOFTREE_TEST_CONFIG_FILE
#define ROOFTREE_TEST_CONFIG_FILE "rooftree_test.yaml"
#endif

namespace Rooftree::Test
{
// nullopt with the reason to skip in why: no file, or Host/User/Name not filled in
inline std::optional<DatabaseConfig> GetTestDatabaseConfig(std::string& why)
{
    char const* env = std::getenv("ROOFTREE_TEST_CONFIG");
    std::string file = env && *env ? env : ROOFTREE_TEST_CONFIG_FILE;

    std::shared_ptr<Config const> config;
    try
    {
        config = Config::Parse(YAML::LoadFile(file), file);
    }
    catch (YAML::Exception const& e)
    {
        why = "could not load " + file + ": " + e.what();
        return std::nullopt;
    }

    std::optional<DatabaseConfig> database = config->Get<DatabaseConfig>("Database.Test");
    if (!database)
        why = "Database.Test in " + file + " is not set or invalid (needs Host, User and Name)";
    return database;
}
}

#endif // ROOFTREE_TESTDATABASECONFIG_H
