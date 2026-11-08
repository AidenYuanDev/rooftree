/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A steady clock timer on an IoContext's executor (see IoContext.h for why not steady_timer).
 */

#ifndef ROOFTREE_DEADLINE_TIMER_H
#define ROOFTREE_DEADLINE_TIMER_H

#include "IoContext.h"
#include <boost/asio/basic_waitable_timer.hpp>
#include <chrono>

namespace Rooftree::Asio
{
class DeadlineTimer : public boost::asio::basic_waitable_timer<std::chrono::steady_clock, boost::asio::wait_traits<std::chrono::steady_clock>,
    IoContextExecutor>
{
public:
    using basic_waitable_timer::basic_waitable_timer;
};
}

#endif // ROOFTREE_DEADLINE_TIMER_H
