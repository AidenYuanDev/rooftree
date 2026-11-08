/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Catch2 for the tests, and how they print an Optional in a failure: its value, or <empty>. Only for
 * values that print themselves, the rest shows as {?}.
 */

#ifndef ROOFTREE_CATCH2_H
#define ROOFTREE_CATCH2_H

#include "Optional.h"
#include <ostream>

template<typename T> requires requires(std::ostream& out, T const& value) { out << value; }
std::ostream& operator<<(std::ostream& out, Optional<T> const& value)
{
    if (value)
        return out << "Opt { " << *value << " }";
    return out << "Opt (<empty>)";
}

inline std::ostream& operator<<(std::ostream& out, std::nullopt_t)
{
    return out << "<empty>";
}

#include <catch2/catch_all.hpp>

#endif // ROOFTREE_CATCH2_H
