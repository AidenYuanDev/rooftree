/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * What may fail for a reason the caller shows (a file missing, a config that doesn't parse): the value,
 * or the reason as text.
 *
 *   Result<std::string> ReadWholeFile(path);           // the content, or why not
 *   Result<> Load();                                   // nothing, or why not
 *
 *   if (Result<> loaded = sConfigMgr->LoadFile(file); !loaded)
 *       RT_LOG_ERROR("server", "{}", loaded.error());
 *
 * Failing: return std::unexpected("why"). The monadic and_then, transform and or_else chain steps.
 */

#ifndef ROOFTREE_RESULT_H
#define ROOFTREE_RESULT_H

#include <expected>
#include <string>

namespace Rooftree
{
template<typename T = void>
using Result = std::expected<T, std::string>;
}

#endif // ROOFTREE_RESULT_H
