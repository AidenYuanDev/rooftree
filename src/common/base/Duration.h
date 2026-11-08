/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Short names for the durations and clocks used everywhere, and the chrono literals (5s, 100ms).
 * TimePoint is the steady clock's, for timeouts and intervals; SystemTimePoint the wall clock's, for
 * what is stored or shown.
 */

#ifndef ROOFTREE_DURATION_H
#define ROOFTREE_DURATION_H

#include <chrono>

using Milliseconds = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;
using Minutes = std::chrono::minutes;
using Hours = std::chrono::hours;

using TimePoint = std::chrono::steady_clock::time_point;
using SystemTimePoint = std::chrono::system_clock::time_point;

using namespace std::chrono_literals;

#endif // ROOFTREE_DURATION_H
