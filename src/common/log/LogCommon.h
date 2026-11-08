/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The numbers of the log config (examples/rooftree.yaml.dist): levels, appender types and flags.
 */

#ifndef ROOFTREE_LOG_COMMON_H
#define ROOFTREE_LOG_COMMON_H

#include "Define.h"
#include <string_view>

enum LogLevel : uint8
{
    LOG_LEVEL_DISABLED  = 0,
    LOG_LEVEL_TRACE     = 1,
    LOG_LEVEL_DEBUG     = 2,
    LOG_LEVEL_INFO      = 3,
    LOG_LEVEL_WARN      = 4,
    LOG_LEVEL_ERROR     = 5,
    LOG_LEVEL_FATAL     = 6,
    LOG_LEVEL_INVALID   = 0xFF
};

enum AppenderType : uint8
{
    APPENDER_NONE       = 0,
    APPENDER_CONSOLE    = 1,
    APPENDER_FILE       = 2,
    APPENDER_DB         = 3,
};

enum AppenderFlags : uint8
{
    APPENDER_FLAGS_NONE                 = 0x00,
    APPENDER_FLAGS_PREFIX_TIMESTAMP     = 0x01,
    APPENDER_FLAGS_PREFIX_LOGLEVEL      = 0x02,
    APPENDER_FLAGS_PREFIX_LOGFILTERTYPE = 0x04,
    APPENDER_FLAGS_USE_TIMESTAMP        = 0x08,     // file: the start time in the file's name
    APPENDER_FLAGS_MAKE_FILE_BACKUP     = 0x10,     // file: rename the old one instead of overwriting it (mode w)
};

// "INFO", "ERROR", ...
std::string_view LogLevelName(LogLevel level);

#endif // ROOFTREE_LOG_COMMON_H
