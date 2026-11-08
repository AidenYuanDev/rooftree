/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * What the status page (/zh/status, "看云") shows: how the server is doing now, and a sample a minute
 * (Blog.Status.SampleInterval) for the last 24 hours, 1440 of them. In memory only, a restart starts over.
 *
 * A sample: readers (people, as the footer counts them; the most within the minute), open pages,
 * requests in the minute (every one the process was sent, static files included), how long the World's
 * updates took (average, longest), the process's memory and CPU, the host's load and memory, free disk.
 * The numbers come from /proc and the file system, read on the main thread once a minute and per status request
 * (a few small files); elsewhere than Linux those numbers stay 0.
 *
 * For logged in users only (/api/status takes a login), and even so nothing that says where or what the
 * server is: no addresses, paths, versions or settings.
 *
 * Main thread only, like everything in BlogWorld.
 */

#ifndef BLOG_SERVER_STATUS_H
#define BLOG_SERVER_STATUS_H

#include "Define.h"
#include "Duration.h"
#include "Admin_generated.h"
#include <boost/json/object.hpp>
#include <deque>
#include <string>

namespace Blog
{
class Presence;

struct StatusSample
{
    int64 At = 0;                   // unix seconds, the end of its minute
    uint32 Readers = 0;
    uint32 Pages = 0;
    uint32 Requests = 0;            // per minute
    uint32 UpdateAvgUs = 0;
    uint32 UpdateMaxUs = 0;
    uint32 MemoryMiB = 0;           // the process, resident
    double CpuPercent = 0;          // the process, of all the host's cores
    double Load = 0;                // the host's, over a minute
    uint32 HostMemoryPercent = 0;   // in use
    uint32 DiskFreePercent = 0;     // of the data's file system
};

class ServerStatus
{
public:
    ServerStatus();

    // Where the uploads are: the disk whose free space is shown
    void SetDataDirectory(std::string directory) { _dataDirectory = std::move(directory); }
    // Blog.Status.SampleInterval
    void SetSampleInterval(Milliseconds interval) { _sampleInterval = interval; }
    void SetChatStats(uint64 messagesTotal, uint32 channelsCount)
    {
        _chatMessagesTotal = messagesTotal;
        _chatChannelsCount = channelsCount;
    }
    uint64 GetChatMessagesTotal() const { return _chatMessagesTotal; }
    uint32 GetChatChannelsCount() const { return _chatChannelsCount; }

    // Every update, with how long its work took
    void Update(uint32 diff, std::chrono::microseconds updateTime, Presence const& presence);

    // Now, and the samples after since (unix seconds, 0: all); "requests" is per minute either way
    boost::json::object ToJson(int64 since, Presence const& presence) const;
    // The same as an admin's Status table (Admin.fbs)
    flatbuffers::Offset<Blog::Admin::Status> BuildTable(flatbuffers::FlatBufferBuilder& builder, int64 since, Presence const& presence) const;

    Seconds GetUptime() const;
    std::deque<StatusSample> const& GetSamples() const { return _samples; }
    // Now: what the minute so far makes of it
    StatusSample Probe(Presence const& presence) const;

private:
    void TakeSample(Presence const& presence);

    std::string _dataDirectory;
    Milliseconds _sampleInterval = 1min;
    TimePoint _start;
    std::deque<StatusSample> _samples;

    // The minute being counted
    TimePoint _minuteStart;
    Milliseconds _sinceSample = 0ms;
    uint64 _requestsBefore = 0;
    uint64 _cpuTicksBefore = 0;
    uint32 _updates = 0;
    std::chrono::microseconds _updateTotal{};
    std::chrono::microseconds _updateMax{};
    uint32 _readersPeak = 0;
    uint64 _chatMessagesTotal = 0;
    uint32 _chatChannelsCount = 0;
};
}

#endif // BLOG_SERVER_STATUS_H
