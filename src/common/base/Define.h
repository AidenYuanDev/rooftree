/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The integer names everything uses (uint32 for std::uint32_t and the like).
 */

#ifndef ROOFTREE_DEFINE_H
#define ROOFTREE_DEFINE_H

// valgrind --tool=helgrind (cmake -DHELGRIND=ON): libstdc++ marks where its shared_ptr and the like
// synchronize; these send the marks to helgrind. Before any standard header
#ifdef HELGRIND
#include <valgrind/helgrind.h>
#undef _GLIBCXX_SYNCHRONIZATION_HAPPENS_BEFORE
#undef _GLIBCXX_SYNCHRONIZATION_HAPPENS_AFTER
#define _GLIBCXX_SYNCHRONIZATION_HAPPENS_BEFORE(address) ANNOTATE_HAPPENS_BEFORE(address)
#define _GLIBCXX_SYNCHRONIZATION_HAPPENS_AFTER(address) ANNOTATE_HAPPENS_AFTER(address)
#endif

#include <cstddef>
#include <cstdint>

using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

// A function taking a printf format at argument formatIndex and its arguments from firstIndex on: the
// compiler checks them
#define ROOFTREE_PRINTF(formatIndex, firstIndex) __attribute__((format(printf, formatIndex, firstIndex)))

#endif // ROOFTREE_DEFINE_H
