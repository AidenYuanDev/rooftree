/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "rooftree_catch2.h"

#include "StringConvert.h"
#include <limits>

using Rooftree::StringTo;

TEST_CASE("StringTo reads the whole text as an integer or nothing", "[StringConvert]")
{
    CHECK(StringTo<uint32>("42") == 42u);
    CHECK(StringTo<int32>("-42") == -42);
    CHECK(StringTo<uint32>("0") == 0u);

    // Nothing around the number, no '+'
    for (char const* text : { "", " 42", "42 ", "42tail", "tail42", "+42", "--42", "4 2", "-" })
        CHECK_FALSE(StringTo<int32>(text));
    CHECK_FALSE(StringTo<uint32>("-1"));
}

TEST_CASE("StringTo checks the range of the type", "[StringConvert]")
{
    CHECK(StringTo<uint8>("255") == 255);
    CHECK_FALSE(StringTo<uint8>("256"));
    CHECK(StringTo<int8>("-128") == -128);
    CHECK_FALSE(StringTo<int8>("128"));
    CHECK(StringTo<uint16>("65535") == 65535);
    CHECK_FALSE(StringTo<int16>("32768"));
    CHECK(StringTo<uint32>("4294967295") == std::numeric_limits<uint32>::max());
    CHECK_FALSE(StringTo<uint32>("4294967296"));
    CHECK(StringTo<int64>("-9223372036854775808") == std::numeric_limits<int64>::min());
    CHECK(StringTo<uint64>("18446744073709551615") == std::numeric_limits<uint64>::max());
    CHECK_FALSE(StringTo<uint64>("18446744073709551616"));
}

TEST_CASE("StringTo in another base, or the base of a 0x or 0b prefix", "[StringConvert]")
{
    CHECK(StringTo<uint32>("ff", 16) == 0xFFu);
    CHECK(StringTo<uint32>("FF", 16) == 0xFFu);
    CHECK(StringTo<uint32>("101010", 2) == 0b101010u);
    CHECK(StringTo<int32>("-7f", 16) == -0x7F);
    CHECK_FALSE(StringTo<uint32>("100000000", 16));

    // A prefix only with base 0, which reads decimal without one
    CHECK_FALSE(StringTo<uint32>("0xff"));
    CHECK(StringTo<uint32>("0xff", 0) == 0xFFu);
    CHECK(StringTo<uint32>("0XFF", 0) == 0xFFu);
    CHECK(StringTo<uint32>("0b101010", 0) == 0b101010u);
    CHECK(StringTo<uint32>("42", 0) == 42u);
    CHECK(StringTo<uint32>("0xffffffff", 0) == 0xFFFFFFFFu);
    CHECK_FALSE(StringTo<uint32>("0x100000000", 0));
    CHECK_FALSE(StringTo<uint32>("0x", 0));
    CHECK_FALSE(StringTo<uint8>("0x1ff", 0));
}

TEST_CASE("StringTo reads decimal and scientific floating point numbers", "[StringConvert]")
{
    CHECK(StringTo<double>("0.5") == 0.5);
    CHECK(StringTo<double>("-2") == -2.0);
    CHECK(StringTo<double>("1e+5") == 100000.0);
    CHECK(StringTo<double>("2.5e-3") == 0.0025);
    CHECK(StringTo<float>("1.5") == 1.5f);

    for (char const* text : { "", "1.2.3", "1e+3+5", "a1.5", "1.5tail", " 1", "0x1p3" })
        CHECK_FALSE(StringTo<double>(text));
}

TEST_CASE("StringTo reads the usual words for bool", "[StringConvert]")
{
    for (char const* yes : { "1", "true", "TRUE", "yes", "Yes", "on", "y" })
        CHECK(StringTo<bool>(yes) == true);
    for (char const* no : { "0", "false", "no", "OFF", "n" })
        CHECK(StringTo<bool>(no) == false);
    for (char const* neither : { "", "2", "maybe", "true " })
        CHECK_FALSE(StringTo<bool>(neither));
}
