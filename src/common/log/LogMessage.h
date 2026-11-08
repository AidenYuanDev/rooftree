/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#ifndef ROOFTREE_LOG_MESSAGE_H
#define ROOFTREE_LOG_MESSAGE_H

#include "LogCommon.h"
#include <ctime>
#include <string>

struct LogMessage
{
    LogLevel Level;
    std::string Type;       // the logger's name it was written to, "server.http"
    std::string Text;
    time_t Time;

    // Local time, 2026-11-07_19:31:48
    std::string FormatTime() const;
};

#endif // ROOFTREE_LOG_MESSAGE_H
