/*
 * Logging cost on the calling thread: filtered messages (should be close to free), logger lookup,
 * synchronous file writes from one or many threads, and the async mode where the caller only posts
 * the message to an io_context.
 *
 * Writes <temp dir>/rooftree_bench.log, no console output.
 */

#include "rooftree_benchmark.h"

#include "Config.h"
#include "IoContext.h"
#include "Log.h"
#include "StringFormat.h"
#include <boost/asio/executor_work_guard.hpp>
#include <filesystem>
#include <optional>

namespace
{
void LoadLogConfig(std::string const& loggers, std::string const& fileAppender = "2,3,0,rooftree_bench.log,w")
{
    std::string error;
    REQUIRE(sConfigMgr->LoadString(Rooftree::StringFormat(R"(
        LogsDir: "{}"
        Appender:
          File: "{}"
        Logger: {{ {} }}
    )", std::filesystem::temp_directory_path().generic_string(), fileAppender, loggers), error));
    sLog->LoadFromConfig();
}

// Leaves logging in a quiet state for the following test cases
void DisableLogging()
{
    LoadLogConfig(R"(root: "6,File")");
}
}

TEST_CASE("Log filtering", "[log]")
{
    // root WARN, "bench.trace" TRACE: the global threshold drops to TRACE, so the TRACE/DEBUG
    // messages below have to look up their logger before being rejected
    LoadLogConfig(R"(root: "4,File", bench.trace: "1,File")");

    BENCHMARK("ShouldLog, filtered by root")
    {
        return sLog->ShouldLog("bench.other", LOG_LEVEL_DEBUG);
    };

    BENCHMARK("filtered RT_LOG_DEBUG, falls back to root")
    {
        RT_LOG_DEBUG("bench.other", "value {} {}", 1, 2);
        return 0;
    };

    BENCHMARK("filtered RT_LOG_DEBUG, 4 level deep name")
    {
        RT_LOG_DEBUG("bench.other.child.grandchild", "value {} {}", 1, 2);
        return 0;
    };

    BENCHMARK("ShouldLog, enabled logger")
    {
        return sLog->ShouldLog("bench.trace", LOG_LEVEL_TRACE);
    };

    // Nothing below WARN anywhere: TRACE is rejected before any lookup
    LoadLogConfig(R"(root: "4,File")");
    BENCHMARK("filtered RT_LOG_TRACE, global threshold")
    {
        RT_LOG_TRACE("bench.other", "value {} {}", 1, 2);
        return 0;
    };

    DisableLogging();
}

TEST_CASE("Synchronous file logging", "[log]")
{
    LoadLogConfig(R"(root: "3,File")");

    BENCHMARK("RT_LOG_INFO to file, no prefix")
    {
        RT_LOG_INFO("bench.file", "player {} entered map {}", 123456, 571);
        return 0;
    };

    // Timestamp + level + logger name, like a typical Server.log
    LoadLogConfig(R"(root: "3,File")", "2,3,7,rooftree_bench.log,w");
    BENCHMARK("RT_LOG_INFO to file, timestamp/level/name prefix")
    {
        RT_LOG_INFO("bench.file", "player {} entered map {}", 123456, 571);
        return 0;
    };

    for (int threads : Bench::ThreadCounts())
    {
        constexpr int Messages = 10'000;
        BENCHMARK(Bench::Name("10000 messages to one file", threads, "threads"))
        {
            Bench::RunThreads(threads, [threads](int index)
            {
                for (int i = 0; i < Messages / threads; ++i)
                    RT_LOG_INFO("bench.file", "thread {} message {}", index, i);
            });
            return threads;
        };
    }

    DisableLogging();
}

TEST_CASE("Asynchronous file logging", "[log]")
{
    // Asynchronous logging: the caller formats the message and posts it to a strand
    LoadLogConfig(R"(root: "3,File")", "2,3,7,rooftree_bench.log,w");

    for (int threads : Bench::ThreadCounts())
    {
        constexpr int Messages = 10'000;
        BENCHMARK_ADVANCED(Bench::Name("post 10000 messages", threads, "threads"))(Catch::Benchmark::Chronometer meter)
        {
            Rooftree::Asio::IoContext ioContext;
            std::optional<boost::asio::executor_work_guard<Rooftree::Asio::IoContextExecutor>> work(boost::asio::make_work_guard(ioContext));
            std::vector<std::thread> ioThreads;
            for (int i = 0; i < 2; ++i)
                ioThreads.emplace_back([&] { ioContext.run(); });
            sLog->Initialize(&ioContext);

            meter.measure([threads]
            {
                Bench::RunThreads(threads, [threads](int index)
                {
                    for (int i = 0; i < Messages / threads; ++i)
                        RT_LOG_INFO("bench.async", "thread {} message {}", index, i);
                });
                return threads;
            });

            // Drain outside the measurement
            work.reset();
            for (std::thread& ioThread : ioThreads)
                ioThread.join();
            sLog->SetSynchronous();
        };
    }

    DisableLogging();
}

TEST_CASE("Log reload", "[log]")
{
    LoadLogConfig(R"(root: "4,File", server: "3,File", server.http: "2,File", network: "3,File", sql: "4,File")");

    BENCHMARK("LoadFromConfig (5 loggers, 1 file appender)")
    {
        sLog->LoadFromConfig();
        return 0;
    };

    DisableLogging();
}
