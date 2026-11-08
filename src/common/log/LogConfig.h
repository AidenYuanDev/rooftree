/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Lets Appender.* / Logger.* be written as YAML mappings instead of one-line values. A mapping is
 * turned back into its line so Log parses both the same way. The values are the log's numbers, see
 * examples/rooftree.yaml.dist for what they mean:
 *
 *   Appender:
 *     Console:                                     # same as "1,3,7,9 1 8 2 6 7"
 *       Type: 1
 *       Level: 3
 *       Flags: 7
 *       Colors: [9, 1, 8, 2, 6, 7]
 *     Server:                                      # same as "2,2,7,Server.log,w,536870912"
 *       Type: 2
 *       Level: 2
 *       Flags: 7
 *       File: Server.log
 *       Mode: w
 *       MaxSize: 536870912
 *   Logger:
 *     server.http:                                 # same as "2,Console Server"
 *       Level: 2
 *       Appenders: [Console, Server]
 *
 * Other appender types take their optional arguments from Args: [...].
 */

#ifndef ROOFTREE_LOGCONFIG_H
#define ROOFTREE_LOGCONFIG_H

#include <string>

class Config;

namespace LogConfig
{
// The config line for "Appender.<name>": the value itself, or built from its mapping
std::string GetAppenderConfigLine(Config const& config, std::string const& key);

// The config line for "Logger.<name>": the value itself, or built from its mapping
std::string GetLoggerConfigLine(Config const& config, std::string const& key);
}

#endif // ROOFTREE_LOGCONFIG_H
