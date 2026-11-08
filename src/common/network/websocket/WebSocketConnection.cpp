/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "WebSocketConnection.h"
#include "Log.h"
#include <memory>

namespace Rooftree::Net::WebSocket
{
void Connection::Open(ConnectionTransport& /*transport*/)
{
    _connected = true;
    OnOpen();
}

SocketReadCallbackResult Connection::Read(ConnectionTransport& transport, MessageBuffer& buffer)
{
    while (buffer.GetActiveSize() > 0)
    {
        Frame frame;
        CloseCode error = CloseCode::None;
        switch (ParseClientFrame(buffer, frame, _maxMessageSize, error))
        {
            case ParseResult::NeedMoreData:
                return SocketReadCallbackResult::KeepReading;
            case ParseResult::Error:
                Fail(transport, error, "invalid frame");
                return SocketReadCallbackResult::Stop;
            case ParseResult::Done:
                break;
        }

        if (!HandleFrame(transport, std::move(frame)))
            return SocketReadCallbackResult::Stop;
    }

    return SocketReadCallbackResult::KeepReading;
}

void Connection::Flush(ConnectionTransport& transport)
{
    QueuedFrame* frame;
    while (_sendQueue.Dequeue(frame))
    {
        std::unique_ptr<QueuedFrame> owner(frame);

        // Nothing may follow a close frame
        if (_closeSent)
            continue;

        transport.WriteFrame(std::move(frame->Buffer));
        if (frame->CloseAfter)
        {
            _closeSent = true;
            transport.CloseAfterWrites();
        }
    }
}

void Connection::Fail(ConnectionTransport& transport, CloseCode code, std::string_view reason)
{
    RT_LOG_DEBUG("network.websocket", "{} closing: {} ({})", transport.GetPeerInfo(), reason, uint16(code));
    if (!_closeSent)
    {
        _closeSent = true;
        transport.WriteFrame(SerializeCloseFrame(code, reason));
    }
    transport.CloseAfterWrites();
}

bool Connection::HandleFrame(ConnectionTransport& transport, Frame&& frame)
{
    switch (frame.Type)
    {
        case Opcode::Ping:
            if (!_closeSent)
                transport.WriteFrame(SerializeFrame(Opcode::Pong, frame.Payload));
            return true;
        case Opcode::Pong:
            return true;
        case Opcode::Close:
        {
            CloseCode code;
            std::string_view reason;
            if (!ParseClosePayload(frame.Payload, code, reason))
            {
                Fail(transport, CloseCode::ProtocolError, "invalid close frame");
                return false;
            }

            // Answer with the same code, then close the TCP connection (the server side closes first)
            RT_LOG_DEBUG("network.websocket", "{} closed by peer: {} {}", transport.GetPeerInfo(), uint16(code), reason);
            if (!_closeSent)
            {
                _closeSent = true;
                transport.WriteFrame(SerializeCloseFrame(code, {}));
            }
            transport.CloseAfterWrites();
            return false;
        }
        case Opcode::Text:
        case Opcode::Binary:
            if (_fragmentType != Opcode::Continuation)
            {
                Fail(transport, CloseCode::ProtocolError, "new message before the previous one ended");
                return false;
            }

            if (frame.Fin)
                return Deliver(transport, frame.Type, std::move(frame.Payload));

            _fragmentType = frame.Type;
            _fragments = std::move(frame.Payload);
            return true;
        case Opcode::Continuation:
            if (_fragmentType == Opcode::Continuation)
            {
                Fail(transport, CloseCode::ProtocolError, "continuation without a message");
                return false;
            }

            if (_fragments.size() + frame.Payload.size() > _maxMessageSize)
            {
                Fail(transport, CloseCode::MessageTooBig, "message too big");
                return false;
            }

            _fragments += frame.Payload;
            if (!frame.Fin)
                return true;

            return Deliver(transport, std::exchange(_fragmentType, Opcode::Continuation), std::exchange(_fragments, {}));
    }

    return true;
}

bool Connection::Deliver(ConnectionTransport& transport, Opcode opcode, std::string&& payload)
{
    if (opcode == Opcode::Text && !IsValidUtf8(payload))
    {
        Fail(transport, CloseCode::InvalidPayload, "text message is not UTF-8");
        return false;
    }

    // Nothing after our close frame is answered
    if (!_closeSent)
        OnMessage(opcode, std::move(payload));

    return transport.IsTransportOpen();
}
}
