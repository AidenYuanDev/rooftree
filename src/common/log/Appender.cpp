/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Appender.h"
#include "LogMessage.h"
#include "StringFormat.h"

std::string_view LogLevelName(LogLevel level)
{
    switch (level)
    {
        case LOG_LEVEL_TRACE: return "TRACE";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO: return "INFO";
        case LOG_LEVEL_WARN: return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_FATAL: return "FATAL";
        default: return "DISABLED";
    }
}

std::string LogMessage::FormatTime() const
{
    tm local;
    localtime_r(&Time, &local);
    return Rooftree::StringFormat("{:04}-{:02}-{:02}_{:02}:{:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
        local.tm_hour, local.tm_min, local.tm_sec);
}

void Appender::Append(LogMessage const& message)
{
    LogLevel level = _level;
    if (level == LOG_LEVEL_DISABLED || message.Level < level)
        return;

    // 2026-11-07_19:31:48 INFO  [server.http] text
    std::string prefix;
    if (_flags & APPENDER_FLAGS_PREFIX_TIMESTAMP)
        prefix += message.FormatTime() + ' ';
    if (_flags & APPENDER_FLAGS_PREFIX_LOGLEVEL)
        prefix += Rooftree::StringFormat("{:<5}", LogLevelName(message.Level));
    if (_flags & APPENDER_FLAGS_PREFIX_LOGFILTERTYPE)
        prefix += Rooftree::StringFormat("[{}] ", message.Type);

    std::scoped_lock lock(_writing);
    if (!_retired)
        Write(message, prefix);
}

void Appender::Retire()
{
    std::scoped_lock lock(_writing);
    _retired = true;
    Close();
}
