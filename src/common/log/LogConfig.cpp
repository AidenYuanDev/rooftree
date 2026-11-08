/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "LogConfig.h"
#include "Config.h"
#include "LogCommon.h"
#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
std::string Join(std::vector<std::string> const& values, char separator)
{
    std::string result;
    for (std::string const& value : values)
    {
        if (!result.empty())
            result += separator;
        result += value;
    }
    return result;
}

// A list, or a single value as a one element list: Colors: [9, 1, 8, 2, 6, 7] or Colors: "9 1 8 2 6 7"
std::vector<std::string> GetList(Config const& config, std::string const& path)
{
    if (std::optional<std::vector<std::string>> list = config.Get<std::vector<std::string>>(path))
        return *list;
    if (std::optional<std::string> value = config.Get<std::string>(path))
        return { *value };
    return {};
}

// A misspelled field (Flag: instead of Flags:) would otherwise be ignored without a word. Reported like
// Log reports the rest of a bad config, the appender or logger is still created from the known fields.
void CheckFields(Config const& config, std::string const& key, char const* what, std::initializer_list<std::string_view> known)
{
    for (std::string const& field : config.GetKeys(key))
        if (std::ranges::find(known, field) == known.end())
            fprintf(stderr, "Log::LoadFromConfig: Unknown field '%s' for %s %s\n", field.c_str(), what, key.c_str());
}
}

std::string LogConfig::GetAppenderConfigLine(Config const& config, std::string const& key)
{
    if (!config.IsMap(key))
        return config.Get<std::string>(key, "");

    CheckFields(config, key, "appender", { "Type", "Level", "Flags", "Colors", "File", "Mode", "MaxSize", "Args" });
    std::string type = config.Get<std::string>(key + ".Type", "");
    std::string line = type
        + ',' + config.Get<std::string>(key + ".Level", "")
        + ',' + config.Get<std::string>(key + ".Flags", "0");

    // Optional arguments, in the order the appender expects them
    std::vector<std::string> args = GetList(config, key + ".Args");
    if (args.empty() && type == std::to_string(APPENDER_CONSOLE))
    {
        std::vector<std::string> colors = GetList(config, key + ".Colors");
        if (!colors.empty())
            args.push_back(Join(colors, ' '));
    }
    else if (args.empty() && type == std::to_string(APPENDER_FILE))
    {
        // Without a file name nothing is added, so AppenderFile reports it missing
        std::string file = config.Get<std::string>(key + ".File", "");
        if (file.empty())
            return line;

        args.push_back(file);
        std::string mode = config.Get<std::string>(key + ".Mode", "");
        std::string maxSize = config.Get<std::string>(key + ".MaxSize", "");
        if (!mode.empty() || !maxSize.empty())
            args.push_back(mode.empty() ? "a" : mode); // AppenderFile's default
        if (!maxSize.empty())
            args.push_back(maxSize);
    }

    for (std::string const& arg : args)
        line += ',' + arg;
    return line;
}

std::string LogConfig::GetLoggerConfigLine(Config const& config, std::string const& key)
{
    if (!config.IsMap(key))
        return config.Get<std::string>(key, "");

    CheckFields(config, key, "logger", { "Level", "Appenders" });
    return config.Get<std::string>(key + ".Level", "")
        + ',' + Join(GetList(config, key + ".Appenders"), ' ');
}
