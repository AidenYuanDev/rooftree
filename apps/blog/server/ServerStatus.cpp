/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "ServerStatus.h"
#include "Presence.h"
#include "WorldHttpService.h"
#include <boost/json/array.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace Blog
{
namespace
{
constexpr std::size_t SampleCount = 24 * 60;

// The process's CPU time so far, in clock ticks: utime and stime of /proc/self/stat. 0 without /proc
uint64 ReadCpuTicks()
{
    std::ifstream file("/proc/self/stat");
    std::string stat((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // The name in parentheses may have spaces, the fields after it don't
    std::size_t end = stat.rfind(')');
    if (end == std::string::npos)
        return 0;

    std::istringstream fields(stat.substr(end + 1));
    std::string field;
    uint64 utime = 0;
    uint64 stime = 0;
    // State is the first, utime the 12th, stime the 13th
    for (int i = 0; i < 11 && fields >> field; ++i) { }
    fields >> utime >> stime;
    return utime + stime;
}

uint32 ReadResidentMiB()
{
    std::ifstream statm("/proc/self/statm");
    uint64 pages = 0;
    uint64 resident = 0;
    if (!(statm >> pages >> resident))
        return 0;
    return uint32(resident * uint64(sysconf(_SC_PAGESIZE)) / (1024 * 1024));
}

double ReadLoad()
{
    std::ifstream loadavg("/proc/loadavg");
    double load = 0;
    loadavg >> load;
    return load;
}

uint32 ReadHostMemoryPercent()
{
    std::ifstream meminfo("/proc/meminfo");
    uint64 total = 0;
    uint64 available = 0;
    std::string key;
    uint64 value = 0;
    std::string unit;
    while (meminfo >> key >> value >> unit)
    {
        if (key == "MemTotal:")
            total = value;
        else if (key == "MemAvailable:")
            available = value;
    }
    return total && available <= total ? uint32((total - available) * 100 / total) : 0;
}

uint32 ReadDiskFreePercent(std::string const& directory)
{
    // Before the first upload the directory may not be there yet: the disk it will be on
    std::error_code error;
    std::filesystem::path path = std::filesystem::absolute(directory.empty() ? "." : directory, error);
    while (!error && !std::filesystem::exists(path, error) && path.has_relative_path())
        path = path.parent_path();
    std::filesystem::space_info space = std::filesystem::space(path, error);
    if (error || !space.capacity || space.capacity == std::uintmax_t(-1))
        return 0;
    return uint32(space.available * 100 / space.capacity);
}

double Round1(double value)
{
    return std::round(value * 10) / 10;
}
}

ServerStatus::ServerStatus() : _start(TimePoint::clock::now()), _minuteStart(_start)
{
    _requestsBefore = Rooftree::Server::WorldHttpService::GetRequestCount();
    _cpuTicksBefore = ReadCpuTicks();
}

Seconds ServerStatus::GetUptime() const
{
    return std::chrono::duration_cast<Seconds>(TimePoint::clock::now() - _start);
}

void ServerStatus::Update(uint32 diff, std::chrono::microseconds updateTime, Presence const& presence)
{
    ++_updates;
    _updateTotal += updateTime;
    _updateMax = std::max(_updateMax, updateTime);
    _readersPeak = std::max(_readersPeak, uint32(presence.GetReaderCount()));

    _sinceSample += Milliseconds(diff);
    if (_sinceSample < _sampleInterval)
        return;

    _sinceSample = 0ms;
    TakeSample(presence);
}

StatusSample ServerStatus::Probe(Presence const& presence) const
{
    TimePoint now = TimePoint::clock::now();
    double seconds = std::chrono::duration<double>(now - _minuteStart).count();
    StatusSample sample = {
        .At = std::chrono::duration_cast<Seconds>(SystemTimePoint::clock::now().time_since_epoch()).count(),
        .Readers = uint32(presence.GetReaderCount()),
        .Pages = uint32(presence.GetViewerCount()),
        .MemoryMiB = ReadResidentMiB(),
        .Load = Round1(ReadLoad()),
        .HostMemoryPercent = ReadHostMemoryPercent(),
        .DiskFreePercent = ReadDiskFreePercent(_dataDirectory),
    };

    // Too little of the minute to tell: the last one's
    if (seconds < std::min(5.0, std::chrono::duration<double>(_sampleInterval).count() / 2) && !_samples.empty())
    {
        StatusSample const& last = _samples.back();
        sample.Requests = last.Requests;
        sample.UpdateAvgUs = last.UpdateAvgUs;
        sample.UpdateMaxUs = last.UpdateMaxUs;
        sample.CpuPercent = last.CpuPercent;
        return sample;
    }

    if (seconds > 0)
    {
        uint64 requests = Rooftree::Server::WorldHttpService::GetRequestCount() - _requestsBefore;
        sample.Requests = uint32(double(requests) * 60 / seconds + 0.5);
        uint64 ticks = ReadCpuTicks() - _cpuTicksBefore;
        double cores = double(std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN)));
        sample.CpuPercent = Round1(double(ticks) / double(sysconf(_SC_CLK_TCK)) / seconds / cores * 100);
    }
    if (_updates)
    {
        sample.UpdateAvgUs = uint32(_updateTotal.count() / _updates);
        sample.UpdateMaxUs = uint32(_updateMax.count());
    }
    return sample;
}

void ServerStatus::TakeSample(Presence const& presence)
{
    StatusSample sample = Probe(presence);
    sample.Readers = std::max(sample.Readers, _readersPeak);
    _samples.push_back(sample);
    if (_samples.size() > SampleCount)
        _samples.pop_front();

    _minuteStart = TimePoint::clock::now();
    _requestsBefore = Rooftree::Server::WorldHttpService::GetRequestCount();
    _cpuTicksBefore = ReadCpuTicks();
    _updates = 0;
    _updateTotal = {};
    _updateMax = {};
    _readersPeak = 0;
}

boost::json::object ServerStatus::ToJson(int64 since, Presence const& presence) const
{
    StatusSample now = Probe(presence);
    boost::json::object current = {
        { "at", now.At },
        { "uptime", GetUptime().count() },
        { "readers", now.Readers },
        { "pages", now.Pages },
        { "requests", now.Requests },
        { "updateAvgUs", now.UpdateAvgUs },
        { "updateMaxUs", now.UpdateMaxUs },
        { "memoryMiB", now.MemoryMiB },
        { "cpuPercent", now.CpuPercent },
        { "load", now.Load },
        { "hostMemoryPercent", now.HostMemoryPercent },
        { "diskFreePercent", now.DiskFreePercent },
        { "cores", std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN)) },
        { "chatMessagesTotal", _chatMessagesTotal },
        { "chatChannelsCount", _chatChannelsCount },
    };

    // By column: a day of samples is one array per number, not 1440 objects with their keys
    boost::json::array at, readers, pages, requests, updateAvg, updateMax, memory, cpu, load, hostMemory, diskFree;
    for (StatusSample const& sample : _samples)
    {
        if (sample.At <= since)
            continue;
        at.push_back(sample.At);
        readers.push_back(sample.Readers);
        pages.push_back(sample.Pages);
        requests.push_back(sample.Requests);
        updateAvg.push_back(sample.UpdateAvgUs);
        updateMax.push_back(sample.UpdateMaxUs);
        memory.push_back(sample.MemoryMiB);
        cpu.push_back(sample.CpuPercent);
        load.push_back(sample.Load);
        hostMemory.push_back(sample.HostMemoryPercent);
        diskFree.push_back(sample.DiskFreePercent);
    }

    return {
        { "now", std::move(current) },
        { "interval", std::chrono::duration_cast<Seconds>(_sampleInterval).count() },
        { "series", {
            { "at", std::move(at) },
            { "readers", std::move(readers) },
            { "pages", std::move(pages) },
            { "requests", std::move(requests) },
            { "updateAvgUs", std::move(updateAvg) },
            { "updateMaxUs", std::move(updateMax) },
            { "memoryMiB", std::move(memory) },
            { "cpuPercent", std::move(cpu) },
            { "load", std::move(load) },
            { "hostMemoryPercent", std::move(hostMemory) },
            { "diskFreePercent", std::move(diskFree) },
        } },
    };
}

flatbuffers::Offset<Blog::Admin::Status> ServerStatus::BuildTable(flatbuffers::FlatBufferBuilder& builder, int64 since, Presence const& presence) const
{
    StatusSample now = Probe(presence);
    auto current = Admin::CreateStatusNow(builder, now.At, GetUptime().count(), now.Readers, now.Pages, now.Requests, now.UpdateAvgUs, now.UpdateMaxUs,
        now.MemoryMiB, now.CpuPercent, now.Load, now.HostMemoryPercent, now.DiskFreePercent, uint32(std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN))),
        _chatMessagesTotal, _chatChannelsCount);

    std::vector<int64> at;
    std::vector<uint32> readers, pages, requests, updateAvg, updateMax, memory, hostMemory, diskFree;
    std::vector<double> cpu, load;
    for (StatusSample const& sample : _samples)
    {
        if (sample.At <= since)
            continue;
        at.push_back(sample.At);
        readers.push_back(sample.Readers);
        pages.push_back(sample.Pages);
        requests.push_back(sample.Requests);
        updateAvg.push_back(sample.UpdateAvgUs);
        updateMax.push_back(sample.UpdateMaxUs);
        memory.push_back(sample.MemoryMiB);
        cpu.push_back(sample.CpuPercent);
        load.push_back(sample.Load);
        hostMemory.push_back(sample.HostMemoryPercent);
        diskFree.push_back(sample.DiskFreePercent);
    }

    auto series = Admin::CreateStatusSeriesDirect(builder, &at, &readers, &pages, &requests, &updateAvg, &updateMax, &memory, &cpu, &load,
        &hostMemory, &diskFree);
    return Admin::CreateStatus(builder, current, std::chrono::duration_cast<Seconds>(_sampleInterval).count(), series);
}
}
