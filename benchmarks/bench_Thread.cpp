/*
 * Queue and thread pool throughput: N producers push a fixed number of items, one consumer drains them,
 * like network threads feeding a session's packet queue. Each run starts its threads, so the batches are
 * large enough to keep thread start-up out of the result.
 */

#include "rooftree_benchmark.h"

#include "LockedQueue.h"
#include "MPSCQueue.h"
#include "ThreadPool.h"
#include <atomic>

namespace
{
constexpr int ItemsPerRun = 100'000;

// Producers push ItemsPerRun values in total, the calling thread pops until it has seen all of them
template <typename Push, typename TryPop>
uint64 ProduceConsume(int producers, Push&& push, TryPop&& tryPop)
{
    int perProducer = ItemsPerRun / producers;
    int total = perProducer * producers;
    uint64 sum = 0;

    std::thread consumer([&]
    {
        int value;
        for (int received = 0; received < total;)
        {
            if (tryPop(value))
            {
                sum += value;
                ++received;
            }
            else
                std::this_thread::yield();
        }
    });

    Bench::RunThreads(producers, [&](int /*index*/)
    {
        for (int i = 0; i < perProducer; ++i)
            push(i);
    });
    consumer.join();
    return sum;
}

// MPSCQueue needs a class type, even for the non-intrusive variant
struct Item
{
    int Value = 0;
};

struct IntrusiveNode
{
    int Value = 0;
    std::atomic<IntrusiveNode*> Next;
};
}

TEST_CASE("Queue push/pop on one thread", "[thread]")
{
    BENCHMARK("LockedQueue")
    {
        LockedQueue<int> queue;
        int value, sum = 0;
        for (int i = 0; i < 1000; ++i)
            queue.add(i);
        while (queue.next(value))
            sum += value;
        return sum;
    };

    BENCHMARK("MPSCQueue (non-intrusive, allocates per item)")
    {
        MPSCQueue<Item> queue;
        Item* item;
        int sum = 0;
        for (int i = 0; i < 1000; ++i)
            queue.Enqueue(new Item{ i });
        while (queue.Dequeue(item))
        {
            sum += item->Value;
            delete item;
        }
        return sum;
    };

    std::vector<IntrusiveNode> nodes(1000);
    BENCHMARK("MPSCQueue (intrusive)")
    {
        MPSCQueue<IntrusiveNode, &IntrusiveNode::Next> queue;
        IntrusiveNode* node;
        int sum = 0;
        for (int i = 0; i < 1000; ++i)
        {
            nodes[i].Value = i;
            queue.Enqueue(&nodes[i]);
        }
        while (queue.Dequeue(node))
            sum += node->Value;
        return sum;
    };
}

TEST_CASE("Queue throughput with several producers", "[thread]")
{
    for (int producers : Bench::ThreadCounts())
    {
        BENCHMARK(Bench::Name("LockedQueue", producers, "producers"))
        {
            LockedQueue<int> queue;
            return ProduceConsume(producers, [&](int i) { queue.add(i); }, [&](int& value) { return queue.next(value); });
        };

        BENCHMARK(Bench::Name("MPSCQueue (non-intrusive)", producers, "producers"))
        {
            MPSCQueue<Item> queue;
            return ProduceConsume(producers, [&](int i) { queue.Enqueue(new Item{ i }); }, [&](int& value)
            {
                Item* item;
                if (!queue.Dequeue(item))
                    return false;
                value = item->Value;
                delete item;
                return true;
            });
        };

        // Preallocated nodes, so only the queue itself is measured (plus one shared counter)
        std::vector<IntrusiveNode> nodes(ItemsPerRun);
        BENCHMARK(Bench::Name("MPSCQueue (intrusive)", producers, "producers"))
        {
            MPSCQueue<IntrusiveNode, &IntrusiveNode::Next> queue;
            std::atomic<int> next = 0;
            return ProduceConsume(producers, [&](int i)
            {
                IntrusiveNode& node = nodes[next++];
                node.Value = i;
                queue.Enqueue(&node);
            }, [&](int& value)
            {
                IntrusiveNode* node;
                if (!queue.Dequeue(node))
                    return false;
                value = node->Value;
                return true;
            });
        };
    }
}

TEST_CASE("ThreadPool task throughput", "[thread]")
{
    for (int threads : Bench::ThreadCounts())
    {
        Rooftree::ThreadPool pool(threads);
        BENCHMARK(Bench::Name("PostWork 10000 tasks", threads, "pool threads"))
        {
            constexpr int Tasks = 10'000;
            std::atomic<int> done = 0;
            for (int i = 0; i < Tasks; ++i)
                pool.PostWork([&done] { done.fetch_add(1, std::memory_order_relaxed); });
            while (done.load(std::memory_order_relaxed) != Tasks)
                std::this_thread::yield();
            return done.load();
        };
        pool.Join();
    }
}
