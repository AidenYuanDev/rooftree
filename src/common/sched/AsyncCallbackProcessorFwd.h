/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#ifndef ROOFTREE_ASYNC_CALLBACK_PROCESSOR_FWD_H
#define ROOFTREE_ASYNC_CALLBACK_PROCESSOR_FWD_H

#include <concepts>

// Something that finishes on another thread: InvokeAsyncCallbackIfReady(callback) runs what follows if it
// did, and says whether it is done with
template<typename T>
concept AsyncCallback = requires(T& callback) { { InvokeAsyncCallbackIfReady(callback) } -> std::convertible_to<bool>; };

template<AsyncCallback T>
class AsyncCallbackProcessor;

#endif // ROOFTREE_ASYNC_CALLBACK_PROCESSOR_FWD_H
