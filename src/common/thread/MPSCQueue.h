/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Lock-free queue of pointers from any number of threads to one: Enqueue(T*) on any thread, Dequeue
 * on one thread only. The queue owns what is in it: what is left is deleted with it.
 *
 * Producers push onto a stack with a compare-and-swap; the consumer takes the whole stack at once with
 * an exchange, turns it round into the order it was pushed in, and pops from that. Each producer's
 * items come out in the order it enqueued them.
 *
 *   MPSCQueue<T>                  a node is allocated per item
 *   MPSCQueue<T, &T::Link>        intrusive: T has a member std::atomic<T*> Link, nothing allocated
 *                                 (SessionMgr's inbox, one message after another)
 */

#ifndef ROOFTREE_MPSC_QUEUE_H
#define ROOFTREE_MPSC_QUEUE_H

#include <atomic>
#include <type_traits>

namespace Rooftree::Impl
{
// Link: how to reach an item's next pointer
template<typename Item, typename Link>
class MPSCStack
{
public:
    MPSCStack() = default;
    MPSCStack(MPSCStack const&) = delete;
    MPSCStack& operator=(MPSCStack const&) = delete;

    void Push(Item* item) noexcept
    {
        Item* top = _top.load(std::memory_order_relaxed);
        do
            Link::Get(item).store(top, std::memory_order_relaxed);
        while (!_top.compare_exchange_weak(top, item, std::memory_order_release, std::memory_order_relaxed));
    }

    // Consumer
    Item* Pop() noexcept
    {
        if (!_ready)
        {
            // Newest first: reversed, oldest first
            Item* taken = _top.exchange(nullptr, std::memory_order_acquire);
            while (taken)
            {
                Item* next = Link::Get(taken).load(std::memory_order_relaxed);
                Link::Get(taken).store(_ready, std::memory_order_relaxed);
                _ready = taken;
                taken = next;
            }
            if (!_ready)
                return nullptr;
        }

        Item* item = _ready;
        _ready = Link::Get(item).load(std::memory_order_relaxed);
        return item;
    }

private:
    std::atomic<Item*> _top = nullptr;
    Item* _ready = nullptr;     // the consumer's, in order
};

template<typename T>
class MPSCQueueNodes
{
public:
    MPSCQueueNodes() = default;
    MPSCQueueNodes(MPSCQueueNodes const&) = delete;
    MPSCQueueNodes& operator=(MPSCQueueNodes const&) = delete;
    ~MPSCQueueNodes()
    {
        T* item;
        while (Dequeue(item))
            delete item;
    }

    void Enqueue(T* item) { _stack.Push(new Node{ item }); }

    bool Dequeue(T*& item)
    {
        Node* node = _stack.Pop();
        if (!node)
            return false;
        item = node->Item;
        delete node;
        return true;
    }

private:
    struct Node
    {
        T* Item;
        std::atomic<Node*> Next = nullptr;
    };

    struct NodeLink
    {
        static std::atomic<Node*>& Get(Node* node) { return node->Next; }
    };

    MPSCStack<Node, NodeLink> _stack;
};

template<typename T, std::atomic<T*> T::* Link>
class MPSCQueueIntrusive
{
public:
    MPSCQueueIntrusive() = default;
    MPSCQueueIntrusive(MPSCQueueIntrusive const&) = delete;
    MPSCQueueIntrusive& operator=(MPSCQueueIntrusive const&) = delete;
    ~MPSCQueueIntrusive()
    {
        T* item;
        while (Dequeue(item))
            delete item;
    }

    void Enqueue(T* item) noexcept { _stack.Push(item); }

    bool Dequeue(T*& item) noexcept
    {
        item = _stack.Pop();
        return item != nullptr;
    }

private:
    struct MemberLink
    {
        static std::atomic<T*>& Get(T* item) { return item->*Link; }
    };

    MPSCStack<T, MemberLink> _stack;
};
}

template<typename T, std::atomic<T*> T::* Link = nullptr>
using MPSCQueue = std::conditional_t<Link == nullptr, Rooftree::Impl::MPSCQueueNodes<T>, Rooftree::Impl::MPSCQueueIntrusive<T, Link>>;

#endif // ROOFTREE_MPSC_QUEUE_H
