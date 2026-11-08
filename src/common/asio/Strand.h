/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * What is posted to a strand runs one at a time, in order, on whichever thread runs the IoContext.
 */

#ifndef ROOFTREE_STRAND_H
#define ROOFTREE_STRAND_H

#include "IoContext.h"
#include <boost/asio/strand.hpp>

namespace Rooftree::Asio
{
class Strand : public boost::asio::strand<IoContextExecutor>
{
public:
    explicit Strand(IoContext& ioContext) : strand(ioContext.get_executor()) { }
};

template<typename Handler>
void post(Strand& strand, Handler&& handler)
{
    boost::asio::post(strand, std::forward<Handler>(handler));
}
}

#endif // ROOFTREE_STRAND_H
