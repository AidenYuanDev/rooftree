/*
 * Shared helpers for the Catch2 benchmarks.
 *
 * Every benchmark returns a value that depends on the work it did, Catch2 keeps such
 * values alive so the optimizer can't drop the measured code.
 */

#ifndef ROOFTREE_BENCHMARK_H
#define ROOFTREE_BENCHMARK_H

#include "Define.h"
#include <catch2/catch_all.hpp>
#include <latch>
#include <string>
#include <thread>
#include <vector>

namespace Bench
{
// Thread counts used by the multi-threaded benchmarks, capped by the machine
inline std::vector<int> ThreadCounts()
{
    int hardware = std::max(1u, std::thread::hardware_concurrency());
    std::vector<int> counts;
    for (int count : { 1, 2, 4, 8 })
        if (count == 1 || count <= hardware)
            counts.push_back(count);
    return counts;
}

// Runs work(index) on `count` threads that are released together, joins them all
template <typename Work>
void RunThreads(int count, Work&& work)
{
    std::latch start(count);
    std::vector<std::thread> threads;
    threads.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        threads.emplace_back([&, i]
        {
            start.arrive_and_wait();
            work(i);
        });
    }
    for (std::thread& thread : threads)
        thread.join();
}

// "name (N things)", Catch2 benchmark names must be unique within a test case
inline std::string Name(std::string_view name, int count, std::string_view unit)
{
    return std::string(name) + " (" + std::to_string(count) + " " + std::string(unit) + ")";
}
}

#endif
