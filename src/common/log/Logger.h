/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#ifndef ROOFTREE_LOGGER_H
#define ROOFTREE_LOGGER_H

#include "Appender.h"
#include "LogMessage.h"
#include <atomic>
#include <string>
#include <vector>

// A name lines are written to ("server.http") with its level, and the appenders they go to
class Logger
{
public:
    Logger(std::string name, LogLevel level, std::vector<Appender*> appenders)
        : _name(std::move(name)), _level(level), _appenders(std::move(appenders)) { }

    std::string const& GetName() const { return _name; }
    LogLevel GetLevel() const { return _level; }
    void SetLevel(LogLevel level) { _level = level; }

    bool IsEnabled(LogLevel level) const
    {
        LogLevel own = _level;
        return own != LOG_LEVEL_DISABLED && level >= own;
    }

    void Write(LogMessage const& message) const
    {
        for (Appender* appender : _appenders)
            appender->Append(message);
    }

private:
    std::string _name;
    std::atomic<LogLevel> _level;
    std::vector<Appender*> _appenders;
};

#endif // ROOFTREE_LOGGER_H
