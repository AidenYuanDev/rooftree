/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Tests of rooftree-common: config, log, base, crypto, schedulers, threads.
 */

#include "rooftree_catch2.h"

#include "Argon2Hash.h"
#include "Config.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "IoContext.h"
#include "Log.h"
#include "LogConfig.h"
#include "MessageBuffer.h"
#include "LockedQueue.h"
#include "MPSCQueue.h"
#include "ProducerConsumerQueue.h"
#include "Random.h"
#include "StringFormat.h"
#include "TaskScheduler.h"
#include "ThreadPool.h"
#include "TOTP.h"
#include "Util.h"
#include <boost/asio/executor_work_guard.hpp>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("Config reads values by path", "[config]")
{
    std::string error;
    REQUIRE(sConfigMgr->LoadString(R"(
        Server:
          Name: rooftree
          Port: 1234
          QuotedPort: "1234"
          Ratio: 1.5
          Enabled: true
          Switch: 1
          Empty:
          Tags: [a, b]

        Logger:
          root: "3,Console"
          server.http: "2,Console"
    )", error));
    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();

    CHECK(config->Get<std::string>("Server.Name", "") == "rooftree");
    CHECK(config->Get<int>("Server.Port", 0) == 1234);
    CHECK(config->Get<uint16>("Server.QuotedPort", 0) == 1234);
    CHECK(config->Get<double>("Server.Ratio", 0.0) == 1.5);
    CHECK(config->Get<bool>("Server.Enabled", false));
    CHECK(config->Get<std::vector<std::string>>("Server.Tags", {}) == std::vector<std::string>{ "a", "b" });
    CHECK(config->Get<std::string>("Server.Port", "") == "1234");
    CHECK(sConfigMgr->Get<int>("Server.Port", 0) == 1234);

    // Missing, empty or not convertible: the default
    CHECK(config->Get<int>("Server.Missing", 42) == 42);
    CHECK_FALSE(config->Get<int>("Server.Missing").has_value());
    CHECK(config->Get<std::string>("Server.Empty", "def") == "def");
    CHECK(config->Get<int>("Server.Name", 42) == 42);
    CHECK(config->Get<int>("Server", 42) == 42);
    CHECK(config->Get<uint8>("Server.Port", 42) == 42);   // out of range
    CHECK_FALSE(config->Get<bool>("Server.Switch").has_value()); // YAML bools are true/false/yes/no/on/off

    // Keys may contain dots, like logger names
    CHECK(config->Get<std::string>("Logger.server.http", "") == "2,Console");
    CHECK(config->GetKeys("Logger") == std::vector<std::string>{ "root", "server.http" });
    CHECK(config->GetKeys("") == std::vector<std::string>{ "Server", "Logger" });
    CHECK(config->GetKeys("Server.Name").empty());
    CHECK(config->Has("Server.Empty"));
    CHECK_FALSE(config->Has("Server.Missing"));
    CHECK(config->IsMap("Logger"));
    CHECK_FALSE(config->IsMap("Logger.root"));
    CHECK(config->GetFilename().empty());
}

TEST_CASE("Config applies environment overrides when it is loaded", "[config]")
{
    std::string error;
    std::string const yaml = "Server: { Port: 1234, Host: }";

    ::setenv("ROOFTREE_SERVER_PORT", "4321", 1);
    ::setenv("ROOFTREE_SERVER_HOST", "example.org", 1);
    ::setenv("ROOFTREE_SERVER_MISSING", "1", 1);
    REQUIRE(sConfigMgr->LoadString(yaml, error));
    ::unsetenv("ROOFTREE_SERVER_PORT");
    ::unsetenv("ROOFTREE_SERVER_HOST");
    ::unsetenv("ROOFTREE_SERVER_MISSING");

    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();
    CHECK(config->Get<int>("Server.Port", 0) == 4321);           // still, the snapshot does not change
    CHECK(config->Get<std::string>("Server.Host", "") == "example.org");
    CHECK(config->GetNode("Server")["Port"].as<int>() == 4321);
    CHECK_FALSE(config->Has("Server.Missing"));                   // only values in the file are overridden

    REQUIRE(sConfigMgr->LoadString(yaml, error));
    CHECK(sConfigMgr->Get<int>("Server.Port", 0) == 1234);
}

TEST_CASE("Config rejects invalid files and keeps the current one", "[config]")
{
    std::string error;
    REQUIRE(sConfigMgr->LoadString("A: 1", error));
    std::shared_ptr<Config const> current = sConfigMgr->GetConfig();

    CHECK_FALSE(sConfigMgr->LoadString("Server: [unclosed", error));
    CHECK(error.find("line 1") != std::string::npos);
    CHECK_FALSE(sConfigMgr->LoadString("this is = not yaml", error)); // a plain scalar, not a mapping
    CHECK_FALSE(sConfigMgr->LoadString("A: 1\nA: 2\n", error));
    CHECK(error.find("duplicate key 'A'") != std::string::npos);
    // Two spellings of the same path
    CHECK_FALSE(sConfigMgr->LoadString("Logger:\n  server.http: x\n  server:\n    http: y\n", error));
    CHECK(error.find("duplicate key 'Logger.server.http'") != std::string::npos);
    CHECK_FALSE(sConfigMgr->LoadFile("/nonexistent/rooftree.yaml", error));
    CHECK(sConfigMgr->GetConfig() == current);

    CHECK_FALSE(sConfigMgr->Reload(error)); // loaded from a string
    CHECK(sConfigMgr->LoadString("", error)); // an empty file is an empty config
    CHECK(sConfigMgr->GetConfig()->GetKeys("").empty());
}

namespace
{
struct RealmInfo
{
    std::string Name;
    uint16 Port = 0;
    std::vector<std::string> Tags;
};
}

template <>
struct YAML::convert<RealmInfo>
{
    static bool decode(Node const& node, RealmInfo& realm)
    {
        if (!node.IsMap() || !node["Name"])
            return false;
        realm.Name = node["Name"].as<std::string>();
        realm.Port = node["Port"].as<uint16>(8085);
        realm.Tags = node["Tags"].as<std::vector<std::string>>(std::vector<std::string>());
        return true;
    }
};

TEST_CASE("Config decodes nested YAML into structs", "[config]")
{
    std::string error;
    REQUIRE(sConfigMgr->LoadString(R"(
        Realms:
          - { Name: one, Port: 8086, Tags: [pvp, rp] }
          - Name: two
        Http:
          Port: 8080
          Limits: { Body: 1024, Header: 64 }
    )", error));
    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();

    std::vector<RealmInfo> realms = config->Get<std::vector<RealmInfo>>("Realms", {});
    REQUIRE(realms.size() == 2);
    CHECK(realms[0].Name == "one");
    CHECK(realms[0].Port == 8086);
    CHECK(realms[0].Tags == std::vector<std::string>{ "pvp", "rp" });
    CHECK(realms[1].Port == 8085);
    CHECK(config->Get<std::map<std::string, int>>("Http.Limits", {}) == std::map<std::string, int>{ { "Body", 1024 }, { "Header", 64 } });
    CHECK_FALSE(config->GetNode("Http.Missing").IsDefined());

    // GetNode returns a copy: changing it does not change the config
    YAML::Node http = config->GetNode("Http");
    http["Port"] = 1;
    http["Missing"];
    CHECK(config->Get<int>("Http.Port", 0) == 8080);
    CHECK_FALSE(config->Has("Http.Missing"));

    // A snapshot keeps its values, reloading only changes what GetConfig returns afterwards
    REQUIRE(sConfigMgr->LoadString("Http: { Port: 9090 }", error));
    CHECK(config->Get<int>("Http.Port", 0) == 8080);
    CHECK(sConfigMgr->Get<int>("Http.Port", 0) == 9090);
}

TEST_CASE("ConfigMgr notifies subscribers of every load", "[config]")
{
    std::filesystem::path file = std::filesystem::temp_directory_path() / "rooftree_test_config.yaml";
    std::ofstream(file) << "Port: 1\n";

    std::string error;
    REQUIRE(sConfigMgr->LoadFile(file.string(), error));

    std::vector<int> ports;
    ConfigMgr::Subscription subscription = sConfigMgr->OnLoaded([&](Config const& config) { ports.push_back(config.Get<int>("Port", 0)); });
    CHECK(ports == std::vector<int>{ 1 }); // called right away with the current config

    std::ofstream(file) << "Port: 2\n";
    REQUIRE(sConfigMgr->Reload(error));
    CHECK(sConfigMgr->GetConfig()->GetFilename() == file.string());

    std::ofstream(file) << "Port: [broken\n";
    CHECK_FALSE(sConfigMgr->Reload(error)); // failed loads notify nobody
    CHECK(ports == std::vector<int>{ 1, 2 });

    ConfigMgr::Subscription moved = std::move(subscription);
    REQUIRE(sConfigMgr->LoadString("Port: 3", error));
    moved.Reset();
    REQUIRE(sConfigMgr->LoadString("Port: 4", error));
    CHECK(ports == std::vector<int>{ 1, 2, 3 });

    std::filesystem::remove(file);
}

TEST_CASE("Config can be reloaded while other threads read it", "[config]")
{
    std::string error;
    REQUIRE(sConfigMgr->LoadString("Http: { Port: 1, Limits: { Body: 1 } }", error));

    std::atomic<bool> stop = false;
    std::atomic<int> reads = 0;
    std::atomic<int> mixed = 0;
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
    {
        readers.emplace_back([&]
        {
            while (!stop)
            {
                // Every config loaded below has Body == Port, two reads of one snapshot never mix two loads
                std::shared_ptr<Config const> config = sConfigMgr->GetConfig();
                if (config->Get<int>("Http.Limits.Body", 0) != config->Get<int>("Http.Port", 0)
                    || config->GetNode("Http")["Limits"]["Body"].as<int>() != config->Get<int>("Http.Port", 0))
                    ++mixed;
                ++reads;
            }
        });
    }

    for (int i = 2; i < 500; ++i)
        REQUIRE(sConfigMgr->LoadString(Rooftree::StringFormat("Http: {{ Port: {0}, Limits: {{ Body: {0} }} }}", i), error));
    stop = true;
    for (std::thread& reader : readers)
        reader.join();

    CHECK(reads > 0);
    CHECK(mixed == 0);
    CHECK(sConfigMgr->Get<int>("Http.Limits.Body", 0) == 499);
}

namespace
{
std::vector<std::string> ReadLines(std::filesystem::path const& file)
{
    std::ifstream in(file);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

// Log config in one-line values, writing to <temp dir>/<file> only (no console noise)
void LoadLogConfig(std::string const& loggers, std::string const& fileAppender = "2,1,0,rooftree_test.log,w")
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
}

TEST_CASE("Log resolves loggers hierarchically", "[log]")
{
    LoadLogConfig(R"(root: "4,File", server: "2,File")");

    CHECK(sLog->ShouldLog("server", LOG_LEVEL_DEBUG));
    CHECK(sLog->ShouldLog("server.http.session", LOG_LEVEL_DEBUG)); // falls back to "server"
    CHECK_FALSE(sLog->ShouldLog("server.http.session", LOG_LEVEL_TRACE));
    CHECK_FALSE(sLog->ShouldLog("network", LOG_LEVEL_INFO));        // falls back to root (warn)
    CHECK(sLog->ShouldLog("network", LOG_LEVEL_ERROR));

    // Nothing is configured below DEBUG, so TRACE is rejected before any logger lookup...
    CHECK_FALSE(sLog->ShouldLog("network", LOG_LEVEL_TRACE));
    // ...until a logger enables it, which lowers that threshold
    sLog->CreateLoggerFromConfigLine("Logger.network", "1,File");
    CHECK(sLog->ShouldLog("network", LOG_LEVEL_TRACE));

    // Like ".server set loglevel"
    CHECK(sLog->SetLogLevel("server", LOG_LEVEL_WARN));
    CHECK_FALSE(sLog->ShouldLog("server.http", LOG_LEVEL_DEBUG));
}

TEST_CASE("Log can be reloaded while other threads log", "[log]")
{
    // Reloading the config calls sLog->LoadFromConfig() while other threads log. It used to destroy the loggers
    // and rebuild the map they were being looked up in. "other" = TRACE keeps the fast path open, so the workers
    // really look up their logger; it stays disabled (root is WARN) to keep the file empty.
    LoadLogConfig(R"(root: "4,File", other: "1,File")");

    std::atomic<bool> stop = false;
    std::atomic<int> calls = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i)
    {
        workers.emplace_back([&]
        {
            while (!stop)
            {
                RT_LOG_INFO("test.reload.child", "message {}", calls.load());
                ++calls;
            }
        });
    }

    // The workers must already be logging, otherwise the reloads can finish before they start
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (calls < 1000 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    int callsBeforeReload = calls;

    for (int i = 0; i < 100; ++i)
        sLog->LoadFromConfig();

    int callsDuringReload = calls - callsBeforeReload;
    stop = true;
    for (std::thread& worker : workers)
        worker.join();
    CHECK(callsBeforeReload >= 1000);
    CHECK(callsDuringReload > 0);
}

TEST_CASE("Log config accepts YAML mappings as well as one-line values", "[log][config]")
{
    std::string error;
    REQUIRE(sConfigMgr->LoadString(R"(
        Appender:
          Line: "1,3,7,9 1 8 2 6 7"
          Console:
            Type: 1
            Level: 3
            Flags: 7
            Colors: [9, 1, 8, 2, 6, 7]
          ColorLine:
            Type: 1
            Level: 3
            Colors: "9 1 8 2 6 7"
          Server:
            Type: 2
            Level: 2
            Flags: 7
            File: Server.log
            MaxSize: 536870912
          Minimal:
            Type: 2
            Level: 3
            File: Minimal.log
          NoFile:
            Type: 2
            Level: 3
          Custom:
            Type: 9
            Level: 3
            Args: [a, b]
        Logger:
          root: "3,Console"
          server.http:
            Level: 2
            Appenders: [Console, Server]
    )", error));

    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();
    CHECK(config->GetKeys("Appender") == std::vector<std::string>{ "Line", "Console", "ColorLine", "Server", "Minimal", "NoFile", "Custom" });

    CHECK(LogConfig::GetAppenderConfigLine(*config, "Appender.Line") == "1,3,7,9 1 8 2 6 7");
    CHECK(LogConfig::GetAppenderConfigLine(*config, "Appender.Console") == "1,3,7,9 1 8 2 6 7");
    CHECK(LogConfig::GetAppenderConfigLine(*config, "Appender.Server") == "2,2,7,Server.log,a,536870912"); // Mode defaults to append
    CHECK(LogConfig::GetAppenderConfigLine(*config, "Appender.ColorLine") == "1,3,0,9 1 8 2 6 7");
    CHECK(LogConfig::GetAppenderConfigLine(*config, "Appender.Minimal") == "2,3,0,Minimal.log");
    CHECK(LogConfig::GetAppenderConfigLine(*config, "Appender.NoFile") == "2,3,0"); // left for AppenderFile to report
    CHECK(LogConfig::GetAppenderConfigLine(*config, "Appender.Custom") == "9,3,0,a,b");
    CHECK(LogConfig::GetLoggerConfigLine(*config, "Logger.root") == "3,Console");
    CHECK(LogConfig::GetLoggerConfigLine(*config, "Logger.server.http") == "2,Console Server");

    // Every field can be overridden on its own
    ::setenv("ROOFTREE_APPENDER_CONSOLE_LEVEL", "4", 1);
    REQUIRE(sConfigMgr->LoadString("Appender: { Console: { Type: 1, Level: 3, Flags: 7 } }", error));
    ::unsetenv("ROOFTREE_APPENDER_CONSOLE_LEVEL");
    CHECK(LogConfig::GetAppenderConfigLine(*sConfigMgr->GetConfig(), "Appender.Console") == "1,4,7");
}

TEST_CASE("Log uses appenders and loggers written as YAML mappings", "[log]")
{
    std::string error;
    REQUIRE(sConfigMgr->LoadString(Rooftree::StringFormat(R"(
        LogsDir: "{}"
        Appender:
          File:
            Type: 2
            Level: 2
            Flags: 6
            File: rooftree_test.log
            Mode: w
        Logger:
          root:
            Level: 4
            Appenders: [File]
          server.http:
            Level: 2
            Appenders: [File]
    )", std::filesystem::temp_directory_path().generic_string()), error));
    sLog->LoadFromConfig();

    CHECK(sLog->ShouldLog("server.http.session", LOG_LEVEL_DEBUG));
    CHECK_FALSE(sLog->ShouldLog("network", LOG_LEVEL_INFO));

    RT_LOG_DEBUG("server.http", "structured");
    CHECK(ReadLines(std::filesystem::temp_directory_path() / "rooftree_test.log") == std::vector<std::string>{ "DEBUG[server.http] structured" });
}

TEST_CASE("File appender keeps lines whole with many writing threads", "[log]")
{
    // Prefix, text and newline written with three separate fwrite calls would let threads interleave between them
    LoadLogConfig(R"(root: "3,File")", "2,3,3,rooftree_test.log,w"); // timestamp + level prefix

    constexpr int ThreadCount = 8;
    constexpr int MessagesPerThread = 2000;
    std::vector<std::thread> writers;
    for (int t = 0; t < ThreadCount; ++t)
        writers.emplace_back([t] { for (int i = 0; i < MessagesPerThread; ++i) RT_LOG_INFO("test.file", "thread {} message {}", t, i); });
    for (std::thread& writer : writers)
        writer.join();

    std::vector<std::string> lines = ReadLines(std::filesystem::temp_directory_path() / "rooftree_test.log");
    CHECK(lines.size() == ThreadCount * MessagesPerThread);
    std::size_t malformed = std::ranges::count_if(lines, [](std::string const& line)
    {
        // "<date>_<time> INFO thread <t> message <i>"
        std::size_t text = line.find(" INFO thread ");
        return text == std::string::npos || line.find(" message ", text) == std::string::npos || line.find(" INFO ", text + 1) != std::string::npos;
    });
    CHECK(malformed == 0);
}

TEST_CASE("Async logging writes every message in order", "[log]")
{
    // Asynchronous logging: messages are posted to a strand on an io_context run by a thread pool
    LoadLogConfig(R"(root: "3,File")");
    Rooftree::Asio::IoContext ioContext;
    std::optional<boost::asio::executor_work_guard<Rooftree::Asio::IoContextExecutor>> work(boost::asio::make_work_guard(ioContext));
    std::vector<std::thread> ioThreads;
    for (int i = 0; i < 2; ++i)
        ioThreads.emplace_back([&] { ioContext.run(); });
    sLog->Initialize(&ioContext);

    constexpr int ThreadCount = 4;
    constexpr int MessagesPerThread = 2000;
    std::vector<std::thread> writers;
    for (int t = 0; t < ThreadCount; ++t)
        writers.emplace_back([t] { for (int i = 0; i < MessagesPerThread; ++i) RT_LOG_INFO("test.async", "{} {}", t, i); });
    for (std::thread& writer : writers)
        writer.join();

    // run() returns once everything queued is written
    work.reset();
    for (std::thread& ioThread : ioThreads)
        ioThread.join();
    sLog->SetSynchronous();

    std::ifstream in(std::filesystem::temp_directory_path() / "rooftree_test.log");
    std::map<int, int> nextPerThread;
    int lines = 0;
    bool inOrder = true;
    for (int t, i; in >> t >> i; ++lines)
    {
        inOrder = inOrder && i == nextPerThread[t];
        nextPerThread[t] = i + 1;
    }
    CHECK(lines == ThreadCount * MessagesPerThread);
    CHECK(inOrder);
}

TEST_CASE("Reloading reopens file appenders", "[log]")
{
    std::filesystem::path file = std::filesystem::temp_directory_path() / "rooftree_test.log";

    SECTION("mode w starts the file over")
    {
        LoadLogConfig(R"(root: "3,File")", "2,3,0,rooftree_test.log,w");
        RT_LOG_INFO("test.reload.file", "before reload");
        sLog->LoadFromConfig();
        RT_LOG_INFO("test.reload.file", "after reload");
        CHECK(ReadLines(file) == std::vector<std::string>{ "after reload" });
    }

    SECTION("mode a keeps appending")
    {
        std::filesystem::remove(file);
        LoadLogConfig(R"(root: "3,File")", "2,3,0,rooftree_test.log,a");
        RT_LOG_INFO("test.reload.file", "before reload");
        sLog->LoadFromConfig();
        RT_LOG_INFO("test.reload.file", "after reload");
        CHECK(ReadLines(file) == std::vector<std::string>{ "before reload", "after reload" });
    }
}

TEST_CASE("MessageBuffer tracks read and write positions", "[base]")
{
    MessageBuffer buffer(8);
    buffer.Write("abcdef", 6);
    CHECK(buffer.GetActiveSize() == 6);
    buffer.ReadCompleted(4);
    CHECK(buffer.GetActiveSize() == 2);
    buffer.Normalize();
    CHECK(std::string_view(reinterpret_cast<char const*>(buffer.GetReadPointer()), buffer.GetActiveSize()) == "ef");
    CHECK(buffer.GetRemainingSpace() == 6);

    // Full: half again as large, what was active stays
    buffer.Write("ghijkl", 6);
    CHECK(buffer.GetRemainingSpace() == 0);
    buffer.EnsureFreeSpace();
    CHECK(buffer.GetBufferSize() == 64);
    CHECK(std::string_view(reinterpret_cast<char const*>(buffer.GetReadPointer()), buffer.GetActiveSize()) == "efghijkl");

    MessageBuffer moved = std::move(buffer);
    CHECK(moved.GetActiveSize() == 8);
    CHECK(buffer.GetActiveSize() == 0);
}

TEST_CASE("Hashes match known vectors", "[crypto]")
{
    using Rooftree::Crypto::SHA1;
    using Rooftree::Crypto::SHA256;
    CHECK(Rooftree::ToHex(SHA1::GetDigestOf(std::string("abc"))) == "A9993E364706816ABA3E25717850C26C9CD0D89D");
    CHECK(Rooftree::ToHex(SHA256::GetDigestOf(std::string("abc"))) == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    CHECK(Rooftree::ToHex(SHA256::GetDigestOf(std::string())) == "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");

    // Parts one after the other; text and bytes alike
    std::vector<uint8> c = { 'c' };
    CHECK(SHA256::GetDigestOf(std::string_view("a"), "b", c) == SHA256::GetDigestOf(std::string("abc")));
    CHECK(SHA256::GetDigestOf(reinterpret_cast<uint8 const*>("abc"), 3) == SHA256::GetDigestOf(std::string("abc")));

    // RFC 6455's example handshake
    CHECK(SHA1::GetDigestOf("dGhlIHNhbXBsZSBub25jZQ==", "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
        == SHA1::Digest{ 0xb3, 0x7a, 0x4f, 0x2c, 0xc0, 0x62, 0x4f, 0x16, 0x90, 0xf6, 0x46, 0x06, 0xcf, 0x38, 0x59, 0x45, 0xb2, 0xbe, 0xc4, 0xea });
}

TEST_CASE("Argon2 hashes carry their parameters and a salt of their own", "[crypto]")
{
    Optional<std::string> hash = Rooftree::Crypto::Argon2::Hash("password", 2, 1024);
    REQUIRE(hash);
    CHECK(hash->starts_with("$argon2id$v=19$m=1024,t=2,p=1$"));
    CHECK(Rooftree::Crypto::Argon2::Verify("password", *hash));
    CHECK_FALSE(Rooftree::Crypto::Argon2::Verify("wrong", *hash));
    CHECK(Rooftree::Crypto::Argon2::Hash("password", 2, 1024) != hash);
    CHECK_FALSE(Rooftree::Crypto::Argon2::Verify("password", "not a hash"));
}

TEST_CASE("Random stays within bounds", "[base]")
{
    for (int i = 0; i < 10000; ++i)
    {
        uint32 u = urand(5, 10);
        CHECK((u >= 5 && u <= 10));
        float f = frand(-1.0f, 1.0f);
        CHECK((f >= -1.0f && f < 1.0f));
        Milliseconds t = randtime(10ms, 20ms);
        CHECK((t >= 10ms && t <= 20ms));
    }
    CHECK(urand(7, 7) == 7);

    // 32 random bits, not all calls should return the same value
    uint32 first = rand32();
    bool differs = false;
    for (int i = 0; i < 100 && !differs; ++i)
        differs = rand32() != first;
    CHECK(differs);
}

TEST_CASE("UTF-8 is counted and cut by code point", "[base]")
{
    std::string text = "h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C"; // "héllo 世界"
    CHECK(Rooftree::Utf8Length(text) == 8);
    CHECK(Rooftree::Utf8Length("\xFF\xFE") == 0);

    std::string cut = text;
    Rooftree::Utf8Truncate(cut, 7);
    CHECK(cut == "h\xC3\xA9llo \xE4\xB8\x96");
    cut = text;
    Rooftree::Utf8Truncate(cut, 100);
    CHECK(cut == text);
    std::string invalid = "a\xFF";
    Rooftree::Utf8Truncate(invalid, 1);
    CHECK(invalid.empty());
}

TEST_CASE("Text helpers: split, hex, case, durations", "[base]")
{
    CHECK(Rooftree::Tokenize("a,,b", ',', true) == std::vector<std::string_view>{ "a", "", "b" });
    CHECK(Rooftree::Tokenize("a,,b,", ',', false) == std::vector<std::string_view>{ "a", "b" });
    CHECK(Rooftree::Tokenize("", ',', true) == std::vector<std::string_view>{ "" });

    std::vector<uint8> bytes = { 0x00, 0xAB, 0x7F };
    CHECK(Rooftree::ToHex(bytes) == "00AB7F");
    CHECK(Rooftree::FromHex("00ab7F") == bytes);
    CHECK_FALSE(Rooftree::FromHex("0"));
    CHECK_FALSE(Rooftree::FromHex("0g"));
    CHECK_FALSE(Rooftree::FromHex("-1"));

    CHECK(Rooftree::StringEqualI("Account", "aCCOUNT"));
    CHECK_FALSE(Rooftree::StringEqualI("Account", "Accounts"));
    CHECK(Rooftree::StringStartsWithI("Account", "acc"));
    CHECK_FALSE(Rooftree::StringStartsWithI("ac", "acc"));

    CHECK(Rooftree::ParseDuration("1d12h") == Seconds(36 * 3600));
    CHECK(Rooftree::ParseDuration("90s") == 90s);
    CHECK(Rooftree::ParseDuration("1h30m") == 90min);
    CHECK_FALSE(Rooftree::ParseDuration("30"));
    CHECK_FALSE(Rooftree::ParseDuration("30m1h"));
    CHECK_FALSE(Rooftree::ParseDuration("1x"));
    CHECK_FALSE(Rooftree::ParseDuration(""));
    CHECK(Rooftree::FormatDuration(Seconds(93784)) == "1d2h3m4s");
    CHECK(Rooftree::FormatDuration(3600s) == "1h");
    CHECK(Rooftree::FormatDuration(0s) == "0s");
}

TEST_CASE("TOTP matches RFC 6238's SHA1 vectors", "[crypto]")
{
    std::string secret = "12345678901234567890";
    std::span<uint8 const> key(reinterpret_cast<uint8 const*>(secret.data()), secret.size());
    // The RFC's 8 digit codes, of which authenticator apps show the last 6
    CHECK(Rooftree::Crypto::TOTP::GenerateToken(key, 59) == 287082);
    CHECK(Rooftree::Crypto::TOTP::GenerateToken(key, 1111111109) == 81804);
    CHECK(Rooftree::Crypto::TOTP::GenerateToken(key, 1234567890) == 5924);
    CHECK(Rooftree::Crypto::TOTP::GenerateToken(key, 2000000000) == 279037);
    // The same within a 30 second step: 1234567890 starts one
    CHECK(Rooftree::Crypto::TOTP::GenerateToken(key, 1234567919) == 5924);
    CHECK(Rooftree::Crypto::TOTP::GenerateToken(key, 1234567889) != 5924);
}

TEST_CASE("TaskScheduler repeats from when a task was due and catches up", "[sched]")
{
    TaskScheduler scheduler;
    std::vector<uint32> runs;
    scheduler.Schedule(100ms, 1, [&](TaskContext context)
    {
        runs.push_back(context.GetRepeatCounter());
        context.Repeat();
    });

    // A single long Update catches up: the task repeats from its own time, 100, 200 and 300 ms
    scheduler.Update(350ms);
    CHECK(runs == std::vector<uint32>{ 0, 1, 2 });
    scheduler.Update(50ms);
    CHECK(runs.size() == 4);

    scheduler.CancelGroup(1);
    scheduler.Update(1s);
    CHECK(runs.size() == 4);
    CHECK(scheduler.Empty());
}

TEST_CASE("TaskScheduler runs in order, moves groups, repeats without a delay at the next update", "[sched]")
{
    TaskScheduler scheduler;
    std::vector<std::string> order;
    scheduler.Schedule(300ms, [&](TaskContext) { order.push_back("c"); });
    scheduler.Schedule(100ms, 2, [&](TaskContext) { order.push_back("a"); });
    scheduler.Schedule(200ms, [&](TaskContext) { order.push_back("b"); });
    scheduler.Update(1s);
    CHECK(order == std::vector<std::string>{ "a", "b", "c" });

    // RescheduleGroup: due from now, whenever it was due
    order.clear();
    scheduler.Schedule(10s, 2, [&](TaskContext) { order.push_back("moved"); });
    scheduler.Schedule(1s, [&](TaskContext) { order.push_back("other"); });
    scheduler.RescheduleGroup(2, 0ms);
    scheduler.Update(1ms);
    CHECK(order == std::vector<std::string>{ "moved" });
    scheduler.Update(1s);
    CHECK(order == std::vector<std::string>{ "moved", "other" });

    int again = 0;
    scheduler.Schedule(0ms, [&](TaskContext context)
    {
        if (++again < 3)
            context.Repeat(0ms);
    });
    scheduler.Update(1ms);
    CHECK(again == 1);
    scheduler.Update(1ms);
    CHECK(again == 2);
    scheduler.Update(1ms);
    scheduler.Update(1ms);
    CHECK(again == 3);

    // A task may schedule another, counted from its own time
    order.clear();
    scheduler.Schedule(100ms, [&](TaskContext) { scheduler.Schedule(100ms, [&](TaskContext) { order.push_back("inner"); }); });
    scheduler.Update(250ms);
    CHECK(order == std::vector<std::string>{ "inner" });
}

namespace
{
struct QueuedItem
{
    int Producer = 0;
    int Number = 0;
    std::atomic<QueuedItem*> Link = nullptr;
};

template<typename Queue>
void CheckOrderPerProducer()
{
    constexpr int Producers = 8, PerProducer = 20000;
    Queue queue;
    std::vector<std::thread> producers;
    for (int p = 0; p < Producers; ++p)
        producers.emplace_back([&queue, p] { for (int i = 0; i < PerProducer; ++i) queue.Enqueue(new QueuedItem{ .Producer = p, .Number = i }); });

    std::vector<int> next(Producers, 0);
    int taken = 0;
    bool inOrder = true;
    while (taken < Producers * PerProducer)
    {
        QueuedItem* item;
        if (!queue.Dequeue(item))
            continue;
        inOrder &= item->Number == next[item->Producer]++;
        delete item;
        ++taken;
    }
    for (std::thread& producer : producers)
        producer.join();

    QueuedItem* left;
    CHECK_FALSE(queue.Dequeue(left));
    CHECK(inOrder);
    // What is left is deleted with the queue (ASan checks)
    queue.Enqueue(new QueuedItem());
}
}

TEST_CASE("MPSCQueue keeps each producer's order", "[thread]")
{
    SECTION("a node per item") { CheckOrderPerProducer<MPSCQueue<QueuedItem>>(); }
    SECTION("intrusive") { CheckOrderPerProducer<MPSCQueue<QueuedItem, &QueuedItem::Link>>(); }
}

TEST_CASE("LockedQueue: each thread takes the front item its filter accepts, readd puts items back", "[thread]")
{
    // A session's packets, the map's (movement) and the world's (chat) mixed
    struct Packet
    {
        bool ForMap;
        int Number;
    };
    LockedQueue<Packet> queue;
    for (int i = 0; i < 6; ++i)
        queue.add({ .ForMap = i % 3 != 2, .Number = i });

    auto map = [](Packet const& packet) { return packet.ForMap; };
    auto world = [](Packet const& packet) { return !packet.ForMap; };
    std::vector<std::string> handled;
    Packet packet;
    // Whichever thread polls, a packet waits until the one before it is taken: the session's order holds
    for (int round = 0; round < 6; ++round)
    {
        while (queue.next(packet, map))
            handled.push_back("map " + std::to_string(packet.Number));
        while (queue.next(packet, world))
            handled.push_back("world " + std::to_string(packet.Number));
    }
    CHECK(handled == std::vector<std::string>{ "map 0", "map 1", "world 2", "map 3", "map 4", "world 5" });
    CHECK(queue.empty());

    // Taken, then found not ready yet: back in front, in order
    queue.add({ .ForMap = true, .Number = 9 });
    std::vector<Packet> notYet = { { true, 7 }, { true, 8 } };
    queue.readd(notYet.begin(), notYet.end());
    std::vector<int> order;
    while (queue.next(packet))
        order.push_back(packet.Number);
    CHECK(order == std::vector<int>{ 7, 8, 9 });
}

TEST_CASE("ProducerConsumerQueue wakes waiting workers, Cancel ends them", "[thread]")
{
    ProducerConsumerQueue<std::unique_ptr<int>> queue;
    std::atomic<int> sum = 0;
    std::atomic<int> ended = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i)
        workers.emplace_back([&]
        {
            std::unique_ptr<int> work;
            while (queue.WaitAndPop(work))
                sum += *work;
            ++ended;
        });

    for (int i = 1; i <= 1000; ++i)
        queue.Push(std::make_unique<int>(i));
    for (int i = 0; i < 2000 && sum < 500500; ++i)
        std::this_thread::sleep_for(1ms);
    CHECK(sum == 500500);
    CHECK(ended == 0);

    // What is still queued is dropped (unique_ptr frees it), every waiter returns false
    queue.Cancel();
    for (std::thread& worker : workers)
        worker.join();
    CHECK(ended == 4);
    queue.Push(std::make_unique<int>(1));
    std::unique_ptr<int> late;
    CHECK_FALSE(queue.Pop(late));
    CHECK(queue.IsCancelled());
}

TEST_CASE("ThreadPool runs all work posted", "[thread]")
{
    std::atomic<int> sum = 0;
    {
        Rooftree::ThreadPool pool(4);
        for (int i = 1; i <= 100; ++i)
            pool.PostWork([&sum, i] { sum += i; });
        pool.Join();
    }
    CHECK(sum == 5050);
}
