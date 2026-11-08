/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A socket's bytes on the way in or out: written at the end, read from the front.
 *
 *   [ read already | active: GetReadPointer(), GetActiveSize() | free: GetWritePointer(), GetRemainingSpace() ]
 *
 * A socket receives into the free part and calls WriteCompleted(bytes), its parser consumes the
 * active part with ReadCompleted(bytes). Normalize() moves what is active to the front to free the
 * space read already; EnsureFreeSpace() grows a buffer that is full.
 */

#ifndef ROOFTREE_MESSAGE_BUFFER_H
#define ROOFTREE_MESSAGE_BUFFER_H

#include "Define.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

class MessageBuffer
{
public:
    explicit MessageBuffer(std::size_t size = 4096) : _bytes(size) { }
    MessageBuffer(MessageBuffer const&) = default;
    MessageBuffer& operator=(MessageBuffer const&) = default;
    MessageBuffer(MessageBuffer&& other) noexcept { *this = std::move(other); }
    MessageBuffer& operator=(MessageBuffer&& other) noexcept
    {
        _bytes = std::move(other._bytes);
        _read = std::exchange(other._read, 0);
        _written = std::exchange(other._written, 0);
        return *this;
    }

    uint8* GetBasePointer() { return _bytes.data(); }
    uint8* GetReadPointer() { return _bytes.data() + _read; }
    uint8 const* GetReadPointer() const { return _bytes.data() + _read; }
    uint8* GetWritePointer() { return _bytes.data() + _written; }

    std::size_t GetActiveSize() const { return _written - _read; }
    std::size_t GetRemainingSpace() const { return _bytes.size() - _written; }
    std::size_t GetBufferSize() const { return _bytes.size(); }

    void ReadCompleted(std::size_t bytes) { _read += bytes; }
    void WriteCompleted(std::size_t bytes) { _written += bytes; }

    // Copies at the end, which has to have room for it
    void Write(void const* data, std::size_t size)
    {
        if (size)
            std::memcpy(GetWritePointer(), data, size);
        _written += size;
    }

    void Reset() { _read = _written = 0; }
    void Resize(std::size_t size) { _bytes.resize(size); }

    void Normalize()
    {
        if (!_read)
            return;
        std::memmove(_bytes.data(), GetReadPointer(), GetActiveSize());
        _written -= _read;
        _read = 0;
    }

    // Half again as large when full
    void EnsureFreeSpace()
    {
        if (!GetRemainingSpace())
            _bytes.resize(std::max<std::size_t>(_bytes.size() * 3 / 2, 64));
    }

private:
    std::vector<uint8> _bytes;
    std::size_t _read = 0;
    std::size_t _written = 0;
};

#endif // ROOFTREE_MESSAGE_BUFFER_H
