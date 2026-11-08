/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Milliseconds since the process started, in 32 bits: they wrap after 49.7 days, so compare two of
 * them by their difference (getMSTimeDiff, or int32(a - b) < 0 for "a is before b"), never with <.
 */

#ifndef ROOFTREE_TIMER_H
#define ROOFTREE_TIMER_H

#include "Define.h"
#include "Duration.h"

inline uint32 getMSTime()
{
    static TimePoint const start = TimePoint::clock::now();
    return uint32(std::chrono::duration_cast<Milliseconds>(TimePoint::clock::now() - start).count());
}

// From earlier to later, also across a wrap
inline uint32 getMSTimeDiff(uint32 earlier, uint32 later)
{
    return later - earlier;
}

#endif // ROOFTREE_TIMER_H
