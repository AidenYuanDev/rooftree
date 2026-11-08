/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "KcpLink.h"
#include "Errors.h"
#include "Timer.h"
#include <ikcp.h>

namespace Rooftree::Net::Kcp
{
namespace
{
// ikcp.c's IKCP_OVERHEAD and IKCP_WND_RCV: a segment's header, and one more than the fragments
// ikcp_send splits a message into at most
constexpr std::size_t SegmentHeaderSize = 24;
constexpr std::size_t MaxFragments = 127;
}

std::size_t Settings::GetMaxMessageSizeLimit() const
{
    // ikcp_recv only sees a message whose fragments are all in its receive queue, which holds ReceiveWindow
    std::size_t fragments = std::min<std::size_t>(MaxFragments, std::size_t(std::max(ReceiveWindow, 128)));
    return fragments * (MaxBodySize - SegmentHeaderSize);
}

Link::Link(uint32 conv, Key const& key, Direction sending, Settings const& settings, Output output, uint64 sendCounter /*= 0*/)
    : _conv(conv), _sending(sending), _maxMessageSize(std::min(settings.MaxMessageSize, settings.GetMaxMessageSizeLimit())),
    _output(std::move(output)), _cipher(key), _sendCounter(sendCounter), _kcp(ikcp_create(conv, this))
{
    ASSERT(_kcp);
    ikcp_setoutput(_kcp, &Link::KcpOutput);
    ikcp_setmtu(_kcp, int(MaxBodySize));
    ikcp_nodelay(_kcp, settings.NoDelay ? 1 : 0, settings.Interval, settings.FastResend, settings.CongestionControl ? 0 : 1);
    ikcp_wndsize(_kcp, settings.SendWindow, settings.ReceiveWindow);
}

Link::~Link()
{
    ikcp_release(_kcp);
}

int Link::KcpOutput(char const* buffer, int length, IKCPCB* /*kcp*/, void* user)
{
    static_cast<Link*>(user)->Send(DatagramType::Data, { reinterpret_cast<uint8 const*>(buffer), std::size_t(length) });
    return 0;
}

bool Link::Open(std::span<uint8 const> datagram, Header const& header, std::size_t clearSize, std::vector<uint8>& body)
{
    if (!_replay.IsNew(header.Counter))
        return false;

    Direction receiving = _sending == Direction::ClientToServer ? Direction::ServerToClient : Direction::ClientToServer;
    if (!_cipher.Open(receiving, datagram, clearSize, body))
        return false;

    _replay.Accept(header.Counter);
    return true;
}

void Link::Send(DatagramType type, std::span<uint8 const> body, std::span<uint8 const> clear /*= {}*/)
{
    _cipher.Seal(_sending, { .Conv = _conv, .Type = type, .Counter = _sendCounter++ }, clear, body, _datagram);
    _output(_datagram);
}

bool Link::SendReliable(std::span<uint8 const> message)
{
    if (message.size() > _maxMessageSize)
        return false;

    return ikcp_send(_kcp, reinterpret_cast<char const*>(message.data()), int(message.size())) >= 0;
}

bool Link::Input(std::span<uint8 const> segments)
{
    return ikcp_input(_kcp, reinterpret_cast<char const*>(segments.data()), long(segments.size())) >= 0;
}

Link::ReceiveResult Link::Receive(std::string& message)
{
    int size = ikcp_peeksize(_kcp);
    if (size < 0)
        return ReceiveResult::None;

    if (std::size_t(size) > _maxMessageSize)
        return ReceiveResult::TooBig;

    message.resize(size);
    int received = ikcp_recv(_kcp, message.data(), size);
    ASSERT(received == size);
    return ReceiveResult::Message;
}

void Link::Flush()
{
    if (!_updated)
        Update(getMSTime());
    else
        ikcp_flush(_kcp);
}

void Link::Update(uint32 now)
{
    ikcp_update(_kcp, now);
    _updated = true;
}

uint32 Link::Check(uint32 now) const
{
    return ikcp_check(_kcp, now);
}

uint32 Link::GetWaitingSegments() const
{
    return uint32(ikcp_waitsnd(_kcp));
}

bool Link::IsDead() const
{
    return _kcp->state != 0;
}
}
