/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "ZipStream.h"
#include <boost/crc.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>

namespace Blog
{
namespace
{
constexpr uint32 LocalHeaderSignature = 0x04034b50;
constexpr uint32 DescriptorSignature = 0x08074b50;
constexpr uint32 CentralSignature = 0x02014b50;
constexpr uint32 Zip64EndSignature = 0x06064b50;
constexpr uint32 Zip64LocatorSignature = 0x07064b50;
constexpr uint32 EndSignature = 0x06054b50;

constexpr uint16 VersionNeeded = 20;
constexpr uint16 VersionNeededZip64 = 45;
constexpr uint16 VersionMadeBy = (3 << 8) | 45;    // Unix, 4.5
// Sizes and CRC in a descriptor after the data, names in UTF-8
constexpr uint16 Flags = 0x0008 | 0x0800;
constexpr uint32 Max32 = std::numeric_limits<uint32>::max();
constexpr uint16 Max16 = std::numeric_limits<uint16>::max();

void Put16(std::string& out, uint16 value)
{
    out += char(value & 0xff);
    out += char(value >> 8);
}

void Put32(std::string& out, uint32 value)
{
    Put16(out, uint16(value & 0xffff));
    Put16(out, uint16(value >> 16));
}

void Put64(std::string& out, uint64 value)
{
    Put32(out, uint32(value & 0xffffffff));
    Put32(out, uint32(value >> 32));
}

// MS-DOS time and date, of the UTC time (the zip has no zone), 1980 at the earliest
std::pair<uint16, uint16> DosTime(SystemTimePoint time)
{
    auto days = std::chrono::floor<std::chrono::days>(time);
    std::chrono::year_month_day date(days);
    std::chrono::hh_mm_ss clock(std::chrono::floor<std::chrono::seconds>(time - days));
    int year = int(date.year());
    if (year < 1980)
        return { 0, (1 << 5) | 1 };
    year = std::min(year, 2107);
    return {
        uint16((clock.hours().count() << 11) | (clock.minutes().count() << 5) | (clock.seconds().count() / 2)),
        uint16(((year - 1980) << 9) | (unsigned(date.month()) << 5) | unsigned(date.day())),
    };
}

struct Placed
{
    ZipEntry Entry;
    uint64 Offset = 0;      // of its local header
    bool Zip64 = false;
    uint32 Crc = 0;
};

bool IsFolder(ZipEntry const& entry) { return entry.Name.ends_with('/'); }

std::string LocalHeader(Placed const& placed)
{
    auto [time, date] = DosTime(placed.Entry.Modified);
    std::string out;
    Put32(out, LocalHeaderSignature);
    Put16(out, placed.Zip64 ? VersionNeededZip64 : VersionNeeded);
    Put16(out, Flags);
    Put16(out, 0);          // stored
    Put16(out, time);
    Put16(out, date);
    Put32(out, 0);          // CRC and sizes: in the descriptor
    Put32(out, placed.Zip64 ? Max32 : 0);
    Put32(out, placed.Zip64 ? Max32 : 0);
    Put16(out, uint16(placed.Entry.Name.size()));
    Put16(out, placed.Zip64 ? 20 : 0);
    out += placed.Entry.Name;
    if (placed.Zip64)
    {
        Put16(out, 0x0001);
        Put16(out, 16);
        Put64(out, 0);
        Put64(out, 0);
    }
    return out;
}

std::string Descriptor(Placed const& placed)
{
    std::string out;
    Put32(out, DescriptorSignature);
    Put32(out, placed.Crc);
    if (placed.Zip64)
    {
        Put64(out, placed.Entry.Size);
        Put64(out, placed.Entry.Size);
    }
    else
    {
        Put32(out, uint32(placed.Entry.Size));
        Put32(out, uint32(placed.Entry.Size));
    }
    return out;
}

std::string CentralHeader(Placed const& placed)
{
    auto [time, date] = DosTime(placed.Entry.Modified);
    bool folder = IsFolder(placed.Entry);
    std::string out;
    Put32(out, CentralSignature);
    Put16(out, VersionMadeBy);
    Put16(out, placed.Zip64 ? VersionNeededZip64 : VersionNeeded);
    Put16(out, Flags);
    Put16(out, 0);
    Put16(out, time);
    Put16(out, date);
    Put32(out, placed.Crc);
    Put32(out, placed.Zip64 ? Max32 : uint32(placed.Entry.Size));
    Put32(out, placed.Zip64 ? Max32 : uint32(placed.Entry.Size));
    Put16(out, uint16(placed.Entry.Name.size()));
    Put16(out, placed.Zip64 ? 28 : 0);
    Put16(out, 0);          // comment
    Put16(out, 0);          // disk
    Put16(out, 0);          // internal attributes
    // rwxr-xr-x folders, rw-r--r-- files; MS-DOS's directory bit
    Put32(out, folder ? (uint32(040755) << 16) | 0x10 : uint32(0100644) << 16);
    Put32(out, placed.Zip64 ? Max32 : uint32(placed.Offset));
    out += placed.Entry.Name;
    if (placed.Zip64)
    {
        Put16(out, 0x0001);
        Put16(out, 24);
        Put64(out, placed.Entry.Size);
        Put64(out, placed.Entry.Size);
        Put64(out, placed.Offset);
    }
    return out;
}

std::string End(uint64 count, uint64 centralOffset, uint64 centralSize)
{
    std::string out;
    bool zip64 = count >= Max16 || centralOffset >= Max32 || centralSize >= Max32;
    if (zip64)
    {
        uint64 zip64EndOffset = centralOffset + centralSize;
        Put32(out, Zip64EndSignature);
        Put64(out, 44);
        Put16(out, VersionMadeBy);
        Put16(out, VersionNeededZip64);
        Put32(out, 0);
        Put32(out, 0);
        Put64(out, count);
        Put64(out, count);
        Put64(out, centralSize);
        Put64(out, centralOffset);

        Put32(out, Zip64LocatorSignature);
        Put32(out, 0);
        Put64(out, zip64EndOffset);
        Put32(out, 1);
    }
    Put32(out, EndSignature);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, zip64 ? Max16 : uint16(count));
    Put16(out, zip64 ? Max16 : uint16(count));
    Put32(out, zip64 ? Max32 : uint32(centralSize));
    Put32(out, zip64 ? Max32 : uint32(centralOffset));
    Put16(out, 0);
    return out;
}

// What a zip is made of, in order: per entry its header, its file, its descriptor; then the central
// directory, built once every CRC is known, and its end
class ZipBuffer final : public std::streambuf
{
public:
    ZipBuffer(std::vector<Placed> entries, uint64 centralOffset, uint64 centralSize)
        : _entries(std::move(entries)), _centralOffset(centralOffset), _centralSize(centralSize), _buffer(BufferSize) { }

protected:
    int_type underflow() override
    {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());

        std::size_t filled = 0;
        while (filled == 0 && !_failed)
        {
            if (_pendingAt < _pending.size())
            {
                filled = std::min(_buffer.size(), _pending.size() - _pendingAt);
                std::memcpy(_buffer.data(), _pending.data() + _pendingAt, filled);
                _pendingAt += filled;
            }
            else if (_fileLeft > 0)
                filled = ReadFile();
            else if (!Advance())
                break;
        }

        if (filled == 0)
            return traits_type::eof();
        setg(_buffer.data(), _buffer.data(), _buffer.data() + filled);
        return traits_type::to_int_type(*gptr());
    }

private:
    static constexpr std::size_t BufferSize = 64 * 1024;

    enum class Step { Header, Data, Descriptor, Central, End, Done };

    std::size_t ReadFile()
    {
        std::size_t wanted = std::size_t(std::min<uint64>(_fileLeft, _buffer.size()));
        _file.read(_buffer.data(), std::streamsize(wanted));
        std::size_t got = std::size_t(_file.gcount());
        if (got != wanted)
        {
            _failed = true;
            return 0;
        }
        _crc.process_bytes(_buffer.data(), got);
        _fileLeft -= got;
        return got;
    }

    void Queue(std::string bytes)
    {
        _pending = std::move(bytes);
        _pendingAt = 0;
    }

    // The next part into _pending (or the next file open); false at the end
    bool Advance()
    {
        switch (_step)
        {
            case Step::Header:
                if (_index == _entries.size())
                {
                    _step = Step::Central;
                    return Advance();
                }
                Queue(LocalHeader(_entries[_index]));
                _step = Step::Data;
                return true;
            case Step::Data:
            {
                Placed& placed = _entries[_index];
                _crc.reset();
                if (!IsFolder(placed.Entry) && placed.Entry.Size > 0)
                {
                    _file = std::ifstream(placed.Entry.Path, std::ios::binary);
                    if (!_file)
                    {
                        _failed = true;
                        return false;
                    }
                    _fileLeft = placed.Entry.Size;
                }
                _step = Step::Descriptor;
                return true;
            }
            case Step::Descriptor:
            {
                _file.close();
                Placed& placed = _entries[_index];
                placed.Crc = _crc.checksum();
                Queue(Descriptor(placed));
                ++_index;
                _step = Step::Header;
                return true;
            }
            case Step::Central:
            {
                std::string central;
                central.reserve(std::size_t(_centralSize));
                for (Placed const& placed : _entries)
                    central += CentralHeader(placed);
                Queue(std::move(central));
                _step = Step::End;
                return true;
            }
            case Step::End:
                Queue(End(_entries.size(), _centralOffset, _centralSize));
                _step = Step::Done;
                return true;
            case Step::Done:
                return false;
        }
        return false;
    }

    std::vector<Placed> _entries;
    uint64 _centralOffset;
    uint64 _centralSize;
    std::vector<char> _buffer;

    Step _step = Step::Header;
    std::size_t _index = 0;
    std::string _pending;
    std::size_t _pendingAt = 0;
    std::ifstream _file;
    uint64 _fileLeft = 0;
    boost::crc_32_type _crc;
    bool _failed = false;
};

class ZipStream final : public std::istream
{
public:
    explicit ZipStream(std::unique_ptr<ZipBuffer> buffer) : std::istream(buffer.get()), _buffer(std::move(buffer)) { }

private:
    std::unique_ptr<ZipBuffer> _buffer;
};
}

std::pair<std::shared_ptr<std::istream>, uint64> MakeZipStream(std::vector<ZipEntry> entries)
{
    // Where each goes, and so the length, before a byte is read: the headers' sizes don't depend on
    // the CRCs
    std::vector<Placed> placed;
    placed.reserve(entries.size());
    uint64 offset = 0;
    for (ZipEntry& entry : entries)
    {
        if (IsFolder(entry))
            entry.Size = 0;
        Placed& next = placed.emplace_back(Placed{ .Entry = std::move(entry), .Offset = offset });
        next.Zip64 = next.Entry.Size >= Max32 || offset >= Max32;
        offset += LocalHeader(next).size() + next.Entry.Size + Descriptor(next).size();
    }
    uint64 centralOffset = offset;
    uint64 centralSize = 0;
    for (Placed const& next : placed)
        centralSize += CentralHeader(next).size();
    uint64 length = centralOffset + centralSize + End(placed.size(), centralOffset, centralSize).size();

    auto buffer = std::make_unique<ZipBuffer>(std::move(placed), centralOffset, centralSize);
    return { std::make_shared<ZipStream>(std::move(buffer)), length };
}
}
