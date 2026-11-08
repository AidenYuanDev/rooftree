/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#ifndef ROOFTREE_CONCEPTS_H
#define ROOFTREE_CONCEPTS_H

#include <concepts>
#include <type_traits>

namespace Rooftree
{
// Callable with Args, giving something that converts to R
template<typename Callable, typename R, typename... Args>
concept invocable_r = std::is_invocable_r_v<R, Callable, Args...>;
}

#endif // ROOFTREE_CONCEPTS_H
