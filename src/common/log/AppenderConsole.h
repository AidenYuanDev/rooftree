/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Lines on stdout, errors and fatal ones on stderr, each flushed: with systemd or Docker stdout is a
 * pipe, buffered until full, so lines would come late and the last ones would be lost to an abort.
 *
 * The fourth value of the config line, optional, colors each level: six numbers for fatal, error,
 * warn, info, debug and trace, from the list in examples/rooftree.yaml.dist (9 is light red).
 */

#ifndef ROOFTREE_APPENDER_CONSOLE_H
#define ROOFTREE_APPENDER_CONSOLE_H

#include "Appender.h"
#include "Optional.h"
#include <array>
#include <string_view>
#include <vector>

class AppenderConsole : public Appender
{
public:
    static constexpr AppenderType Type = APPENDER_CONSOLE;

    AppenderConsole(uint8 id, std::string name, LogLevel level, AppenderFlags flags, std::vector<std::string_view> const& args);

    AppenderType GetType() const override { return Type; }

private:
    void Write(LogMessage const& message, std::string const& prefix) override;

    // Its ANSI escape by level, fatal first
    Optional<std::array<std::string, 6>> _colors;
};

#endif // ROOFTREE_APPENDER_CONSOLE_H
