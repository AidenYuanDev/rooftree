/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "Random.h"
#include "Errors.h"
#include <random>

namespace
{
std::mt19937& Generator()
{
    thread_local std::mt19937 generator = []
    {
        std::random_device device;
        std::seed_seq seed{ device(), device(), device(), device(), device(), device(), device(), device() };
        return std::mt19937(seed);
    }();
    return generator;
}
}

uint32 urand(uint32 min, uint32 max)
{
    ASSERT(min <= max);
    return std::uniform_int_distribution<uint32>(min, max)(Generator());
}

float frand(float min, float max)
{
    ASSERT(min <= max);
    return std::uniform_real_distribution<float>(min, max)(Generator());
}

Milliseconds randtime(Milliseconds min, Milliseconds max)
{
    ASSERT(min <= max);
    return Milliseconds(std::uniform_int_distribution<Milliseconds::rep>(min.count(), max.count())(Generator()));
}

uint32 rand32()
{
    return Generator()();
}
