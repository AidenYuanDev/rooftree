/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Boost.Asio's io_context under a name that can be declared without including Boost.Asio
 * (AsioHacksFwd.h), and the executor type the sockets and timers are made for: a concrete one, not
 * the type-erased any_io_executor, so no call goes through a function pointer.
 */

#ifndef ROOFTREE_IO_CONTEXT_H
#define ROOFTREE_IO_CONTEXT_H

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

namespace Rooftree::Asio
{
class IoContext : public boost::asio::io_context
{
public:
    using io_context::io_context;
};

using IoContextExecutor = boost::asio::io_context::executor_type;
}

#endif // ROOFTREE_IO_CONTEXT_H
