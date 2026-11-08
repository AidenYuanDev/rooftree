/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "KcpProtocol.h"
#include "Errors.h"
#include <openssl/evp.h>
#include <cstring>

namespace Rooftree::Net::Kcp
{
namespace
{
template<typename T>
void WriteLE(uint8* out, T value)
{
    for (std::size_t i = 0; i < sizeof(T); ++i)
        out[i] = uint8(value >> (8 * i));
}

template<typename T>
T ReadLE(uint8 const* in)
{
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= T(in[i]) << (8 * i);
    return value;
}

void MakeNonce(Direction direction, uint64 counter, uint8 (&nonce)[12])
{
    nonce[0] = uint8(direction);
    nonce[1] = nonce[2] = nonce[3] = 0;
    WriteLE(nonce + 4, counter);
}

EVP_CIPHER_CTX* CreateContext(Key const& key, bool encrypting)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    ASSERT(ctx);
    int status = EVP_CipherInit_ex(ctx, EVP_aes_128_gcm(), nullptr, key.data(), nullptr, encrypting ? 1 : 0);
    ASSERT(status);
    return ctx;
}
}

Header ReadHeader(std::span<uint8 const> datagram)
{
    ASSERT(datagram.size() >= HeaderSize);
    return { .Conv = ReadLE<uint32>(datagram.data()), .Type = DatagramType(datagram[4]), .Counter = ReadLE<uint64>(datagram.data() + 5) };
}

std::string_view ReadHelloToken(std::span<uint8 const> datagram, std::size_t& clearSize)
{
    if (datagram.size() < HeaderSize + 1)
        return {};

    std::size_t length = datagram[HeaderSize];
    if (datagram.size() < HeaderSize + 1 + length + TagSize)
        return {};

    clearSize = 1 + length;
    return { reinterpret_cast<char const*>(datagram.data() + HeaderSize + 1), length };
}

std::vector<uint8> MakeCloseBody(uint16 code, std::string_view reason)
{
    reason = reason.substr(0, MaxCloseReasonSize);
    std::vector<uint8> body(2 + reason.size());
    WriteLE(body.data(), code);
    std::memcpy(body.data() + 2, reason.data(), reason.size());
    return body;
}

bool ReadCloseBody(std::span<uint8 const> body, uint16& code, std::string& reason)
{
    if (body.size() < 2 || body.size() > 2 + MaxCloseReasonSize)
        return false;

    code = ReadLE<uint16>(body.data());
    reason.assign(reinterpret_cast<char const*>(body.data() + 2), body.size() - 2);
    return true;
}

Cipher::Cipher(Key const& key) : _seal(CreateContext(key, true)), _open(CreateContext(key, false))
{
}

Cipher::~Cipher()
{
    EVP_CIPHER_CTX_free(_seal);
    EVP_CIPHER_CTX_free(_open);
}

void Cipher::Seal(Direction direction, Header const& header, std::span<uint8 const> clear, std::span<uint8 const> body, std::vector<uint8>& datagram)
{
    std::size_t aadSize = HeaderSize + clear.size();
    datagram.resize(aadSize + body.size() + TagSize);
    uint8* out = datagram.data();
    WriteLE(out, header.Conv);
    out[4] = uint8(header.Type);
    WriteLE(out + 5, header.Counter);
    if (!clear.empty())
        std::memcpy(out + HeaderSize, clear.data(), clear.size());

    // A failing OpenSSL here is out of memory or a broken build, not something to recover from
    uint8 nonce[12];
    MakeNonce(direction, header.Counter, nonce);
    int length = 0;
    int status = EVP_EncryptInit_ex(_seal, nullptr, nullptr, nullptr, nonce);
    status = status && EVP_EncryptUpdate(_seal, nullptr, &length, out, int(aadSize));
    status = status && (body.empty() || EVP_EncryptUpdate(_seal, out + aadSize, &length, body.data(), int(body.size())));
    status = status && EVP_EncryptFinal_ex(_seal, out + aadSize + body.size(), &length);
    status = status && EVP_CIPHER_CTX_ctrl(_seal, EVP_CTRL_GCM_GET_TAG, int(TagSize), out + aadSize + body.size());
    ASSERT(status, "AES-GCM encryption failed");
}

bool Cipher::Open(Direction direction, std::span<uint8 const> datagram, std::size_t clearSize, std::vector<uint8>& body)
{
    std::size_t aadSize = HeaderSize + clearSize;
    if (datagram.size() < aadSize + TagSize)
        return false;

    std::size_t bodySize = datagram.size() - aadSize - TagSize;
    body.resize(bodySize);

    uint8 nonce[12];
    MakeNonce(direction, ReadLE<uint64>(datagram.data() + 5), nonce);
    uint8 tag[TagSize];
    std::memcpy(tag, datagram.data() + aadSize + bodySize, TagSize);

    // GCM's final step writes nothing, it only checks the tag
    uint8 final[16];
    int length = 0;
    return EVP_DecryptInit_ex(_open, nullptr, nullptr, nullptr, nonce)
        && EVP_DecryptUpdate(_open, nullptr, &length, datagram.data(), int(aadSize))
        && (!bodySize || EVP_DecryptUpdate(_open, body.data(), &length, datagram.data() + aadSize, int(bodySize)))
        && EVP_CIPHER_CTX_ctrl(_open, EVP_CTRL_GCM_SET_TAG, int(TagSize), tag)
        && EVP_DecryptFinal_ex(_open, final, &length) > 0;
}
}
