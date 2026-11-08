/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "Errors.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace
{
[[noreturn]] void Report(char const* file, int line, char const* function, char const* what, char const* condition, char const* format,
    va_list* args) noexcept
{
    std::fprintf(stderr, "\n%s:%d in %s: %s%s\n", file, line, function, what, condition);
    if (format)
    {
        std::fputs("  ", stderr);
        std::vfprintf(stderr, format, *args);
        std::fputc('\n', stderr);
    }
    std::fflush(stderr);
    std::abort();
}
}

namespace Rooftree
{
void AssertionFailed(char const* file, int line, char const* function, char const* condition) noexcept
{
    Report(file, line, function, "ASSERTION FAILED: ", condition, nullptr, nullptr);
}

void AssertionFailed(char const* file, int line, char const* function, char const* condition, char const* format, ...) noexcept
{
    va_list args;
    va_start(args, format);
    Report(file, line, function, "ASSERTION FAILED: ", condition, format, &args);
}

void Aborted(char const* file, int line, char const* function) noexcept
{
    Report(file, line, function, "ABORTED", "", nullptr, nullptr);
}

void Aborted(char const* file, int line, char const* function, char const* format, ...) noexcept
{
    va_list args;
    va_start(args, format);
    Report(file, line, function, "ABORTED", "", format, &args);
}
}
