/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Transaction.h"
#include <chrono>

bool TransactionCallback::InvokeIfReady()
{
    if (_future.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
        return false;

    bool const success = _future.get();
    if (_callback)
        _callback(success);
    return true;
}
