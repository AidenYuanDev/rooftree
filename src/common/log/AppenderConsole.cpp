/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "AppenderConsole.h"
#include "LogMessage.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include <cstdio>

namespace
{
// The config's color numbers: black red green brown blue magenta cyan grey, yellow, then bold red green
// blue magenta cyan white
std::string ColorEscape(uint8 color)
{
    static constexpr uint8 Foreground[] = { 30, 31, 32, 33, 34, 35, 36, 37, 33, 31, 32, 34, 35, 36, 37 };
    return Rooftree::StringFormat("\x1b[{}{}m", Foreground[color], color >= 8 ? ";1" : "");
}
}

AppenderConsole::AppenderConsole(uint8 id, std::string name, LogLevel level, AppenderFlags flags, std::vector<std::string_view> const& args)
    : Appender(id, std::move(name), level, flags)
{
    if (args.size() <= 3 || args[3].empty())
        return;

    std::vector<std::string_view> numbers = Rooftree::Tokenize(args[3], ' ', false);
    if (numbers.size() != 6)
        throw InvalidAppenderArgs(Rooftree::StringFormat("Log: console appender {} needs 6 colors (fatal to trace), not '{}'", GetName(), args[3]));

    std::array<std::string, 6> colors;
    for (std::size_t i = 0; i < colors.size(); ++i)
    {
        Optional<uint8> color = Rooftree::StringTo<uint8>(numbers[i]);
        if (!color || *color > 14)
            throw InvalidAppenderArgs(Rooftree::StringFormat("Log: console appender {} has an invalid color '{}'", GetName(), numbers[i]));
        colors[i] = ColorEscape(*color);
    }
    _colors = std::move(colors);
}

void AppenderConsole::Write(LogMessage const& message, std::string const& prefix)
{
    bool error = message.Level >= LOG_LEVEL_ERROR;
    FILE* out = error ? stderr : stdout;
    std::string line = prefix + message.Text;
    if (_colors)
        line = (*_colors)[LOG_LEVEL_FATAL - message.Level] + line + "\x1b[0m";
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), out);
    std::fflush(out);
}
