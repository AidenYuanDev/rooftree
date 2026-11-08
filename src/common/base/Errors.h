/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * What may not happen: ASSERT(condition) or ASSERT(condition, "printf format", arguments...) writes
 * where and what to stderr and aborts, for a core dump (systemd starts the server again). On in every
 * build: they guard what would otherwise corrupt data or memory. ABORT() and ABORT_MSG(...) abort
 * unconditionally (the freeze detector: the main thread hangs).
 *
 * For what can go wrong at run time (a bad request, a file missing) return an error or throw, never
 * assert.
 */

#ifndef ROOFTREE_ERRORS_H
#define ROOFTREE_ERRORS_H

#include "Define.h"

namespace Rooftree
{
[[noreturn]] void AssertionFailed(char const* file, int line, char const* function, char const* condition) noexcept;
[[noreturn]] void AssertionFailed(char const* file, int line, char const* function, char const* condition, char const* format, ...) noexcept
    ROOFTREE_PRINTF(5, 6);
[[noreturn]] void Aborted(char const* file, int line, char const* function) noexcept;
[[noreturn]] void Aborted(char const* file, int line, char const* function, char const* format, ...) noexcept ROOFTREE_PRINTF(4, 5);
}

#define ASSERT(condition, ...) \
    do { if (!(condition)) [[unlikely]] ::Rooftree::AssertionFailed(__FILE__, __LINE__, __func__, #condition __VA_OPT__(,) __VA_ARGS__); } while (0)
#define ABORT() ::Rooftree::Aborted(__FILE__, __LINE__, __func__)
#define ABORT_MSG(...) ::Rooftree::Aborted(__FILE__, __LINE__, __func__, __VA_ARGS__)

#endif // ROOFTREE_ERRORS_H
