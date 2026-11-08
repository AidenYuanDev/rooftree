/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Threads that run what is posted to them, in no particular order: SessionMgr's workers (OnWorker),
 * the arena's. Boost.Asio's thread_pool under a name of ours.
 */

#ifndef ROOFTREE_THREAD_POOL_H
#define ROOFTREE_THREAD_POOL_H

#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <thread>

namespace Rooftree
{
class ThreadPool
{
public:
    explicit ThreadPool(std::size_t threads = std::thread::hardware_concurrency()) : _pool(threads) { }

    // Any thread
    template<typename Work>
    void PostWork(Work&& work) { boost::asio::post(_pool, std::forward<Work>(work)); }

    // Waits for the work posted to be done and the threads to end
    void Join() { _pool.join(); }
    // Work not started yet is dropped
    void Stop() { _pool.stop(); }

private:
    boost::asio::thread_pool _pool;
};
}

#endif // ROOFTREE_THREAD_POOL_H
