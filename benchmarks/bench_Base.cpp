/*
 * Hot helpers from rooftree-common/base and config: socket buffers, formatting, string conversion,
 * random numbers and config lookups.
 */

#include "rooftree_benchmark.h"

#include "Config.h"
#include "MessageBuffer.h"
#include "Random.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include <fmt/format.h>
#include <cstdio>
#include <map>

TEST_CASE("MessageBuffer", "[base]")
{
    // What a socket does per read: compact, append what arrived, consume the whole packets and keep
    // a 100 byte partial one. Write() doesn't grow the buffer, the active size stays at 100 bytes.
    std::array<uint8, 1500> chunk{};
    MessageBuffer buffer(4096);
    BENCHMARK("normalize, write 1500 B, consume all but 100 B")
    {
        buffer.Normalize();
        buffer.Write(chunk.data(), chunk.size());
        buffer.ReadCompleted(buffer.GetActiveSize() - 100);
        return buffer.GetActiveSize();
    };

    BENCHMARK("copy 1500 B into a new buffer (Socket::QueuePacket)")
    {
        MessageBuffer packet(chunk.size());
        packet.Write(chunk.data(), chunk.size());
        Catch::Benchmark::keep_memory(packet.GetReadPointer());
        return packet.GetActiveSize();
    };
}

TEST_CASE("String formatting", "[base]")
{
    uint32 guid = 123456;
    std::string name = "Arthas";
    double ratio = 0.75;

    BENCHMARK("Rooftree::StringFormat")
    {
        return Rooftree::StringFormat("Player {} (guid {}) ratio {:.2f}", name, guid, ratio);
    };

    BENCHMARK("fmt::format")
    {
        return fmt::format("Player {} (guid {}) ratio {:.2f}", name, guid, ratio);
    };

    BENCHMARK("snprintf")
    {
        char buffer[128];
        return std::snprintf(buffer, sizeof(buffer), "Player %s (guid %u) ratio %.2f", name.c_str(), guid, ratio);
    };

    BENCHMARK("Rooftree::StringFormatTo into a reused string")
    {
        static std::string out;
        out.clear();
        Rooftree::StringFormatTo(std::back_inserter(out), "Player {} (guid {}) ratio {:.2f}", name, guid, ratio);
        return out.size();
    };
}

TEST_CASE("StringConvert", "[base]")
{
    // Inputs live outside the benchmarks, literals would be folded at compile time
    std::string uint32Max = "4294967295", int64Min = "-9223372036854775807", garbage = "42tail";
    std::string pi = "3.14159265358979", boolean = "true";

    BENCHMARK("StringTo<uint32>") { return Rooftree::StringTo<uint32>(uint32Max); };
    BENCHMARK("StringTo<int64>") { return Rooftree::StringTo<int64>(int64Min); };
    BENCHMARK("StringTo<uint32> rejects garbage") { return Rooftree::StringTo<uint32>(garbage); };
    BENCHMARK("StringTo<double>") { return Rooftree::StringTo<double>(pi); };
    BENCHMARK("StringTo<bool>") { return Rooftree::StringTo<bool>(boolean); };
}

TEST_CASE("Random", "[base]")
{
    BENCHMARK("urand") { return urand(0, 100); };
    BENCHMARK("frand") { return frand(0.0f, 1.0f); };
    BENCHMARK("rand32") { return rand32(); };
}

TEST_CASE("UTF-8", "[base]")
{
    std::string utf8 = "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C, a typical chat message with some ASCII";
    BENCHMARK("Utf8Length") { return Rooftree::Utf8Length(utf8); };
    BENCHMARK("Utf8Truncate to 20")
    {
        std::string copy = utf8;
        Rooftree::Utf8Truncate(copy, 20);
        return copy.size();
    };
}

TEST_CASE("Config lookups", "[config]")
{
    // Every Get copies the node out of the shared tree before decoding it, settings are meant to be
    // read on load (OnLoaded) and cached, not looked up on hot paths
    std::string error;
    std::string yaml = "Server:\n  Name: rooftree\n  Port: 8085\n  Ratio: 1.5\n  Enabled: true\nRates:\n";
    for (int i = 0; i < 200; ++i)
        yaml += "  Rate" + std::to_string(i) + ": " + std::to_string(i) + "\n";
    REQUIRE(sConfigMgr->LoadString(yaml, error));
    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();

    BENCHMARK("GetConfig") { return sConfigMgr->GetConfig(); };
    BENCHMARK("Get<int>") { return config->Get<int>("Server.Port", 0); };
    BENCHMARK("Get<std::string>") { return config->Get<std::string>("Server.Name", ""); };
    BENCHMARK("Get<double>") { return config->Get<double>("Server.Ratio", 0.0); };
    BENCHMARK("Get<bool>") { return config->Get<bool>("Server.Enabled", false); };
    BENCHMARK("Get<int>, missing key") { return config->Get<int>("Server.Missing", 42); };
    BENCHMARK("Get<int> in a 200 key table") { return config->Get<int>("Rates.Rate150", 0); };
    BENCHMARK("sConfigMgr->Get<int>") { return sConfigMgr->Get<int>("Server.Port", 0); };
    BENCHMARK("GetKeys of a 200 key table") { return config->GetKeys("Rates").size(); };
    BENCHMARK("Get<std::map> of a 200 key table") { return config->Get<std::map<std::string, int>>("Rates", {}).size(); };
    BENCHMARK("LoadString (205 keys)")
    {
        std::string loadError;
        return sConfigMgr->LoadString(yaml, loadError);
    };
}
