/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "IpAddress.h"
#include "StringFormat.h"
#include "rooftree_catch2.h"

using Rooftree::OrNone;
using Rooftree::StringFormat;

TEST_CASE("OrNone formats the value, or (none) without one", "[StringFormat]") {
  CHECK(StringFormat("{}", OrNone(Optional<int>(42))) == "42");
  CHECK(StringFormat("{}", OrNone(Optional<int>())) == "(none)");
  CHECK(StringFormat("{}", OrNone(Optional<std::string>("text"))) == "text");
}

TEST_CASE("OrNone applies the format spec to the value only", "[StringFormat]") {
  CHECK(StringFormat("{:>5}", OrNone(Optional<int>(42))) == "   42");
  CHECK(StringFormat("{:.1f}", OrNone(Optional<double>(2.25))) == "2.2");
  CHECK(StringFormat("{:>5}", OrNone(Optional<int>())) == "(none)");
}

TEST_CASE("StringVFormat says what doesn't fit instead of throwing", "[StringFormat]") {
  int one = 1;
  CHECK(
      Rooftree::StringVFormat("{} and {}", Rooftree::MakeFormatArgs(one))
          .starts_with("(cannot format \"{} and {}\": "));
  CHECK(Rooftree::StringVFormat("{}", Rooftree::MakeFormatArgs(one)) == "1");
}

TEST_CASE("Addresses format as to_string writes them", "[StringFormat]") {
  CHECK(StringFormat("{}", Rooftree::Net::make_address("127.0.0.1")) == "127.0.0.1");
  CHECK(StringFormat("{}", Rooftree::Net::make_address("::1")) == "::1");
  CHECK(StringFormat("[{:>10}]", boost::asio::ip::make_address_v4("10.0.0.1")) == "[  10.0.0.1]");
}
