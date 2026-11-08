/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Rooftree's Boost.Asio classes, declared only: for headers that keep a pointer or a reference and
 * shouldn't pull Boost.Asio into everything that includes them.
 */

#ifndef ROOFTREE_ASIO_FWD_H
#define ROOFTREE_ASIO_FWD_H

namespace Rooftree::Asio
{
class DeadlineTimer;
class IoContext;
class Strand;
}

#endif // ROOFTREE_ASIO_FWD_H
