/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "SslContext.h"
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace Rooftree::Net
{
namespace
{
// TLS 1.2 suites of Mozilla's intermediate configuration without the DHE ones (they need DH parameters);
// TLS 1.3 has its own, all fine
constexpr char const* Tls12Ciphers =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";

std::string LastOpenSslError()
{
    unsigned long code = ERR_get_error();
    ERR_clear_error();
    if (!code)
        return {};

    char buffer[256];
    ERR_error_string_n(code, buffer, sizeof(buffer));
    return buffer;
}
}

std::shared_ptr<boost::asio::ssl::context> CreateServerSslContext(std::string const& certificateChainFile,
    std::string const& privateKeyFile, std::string& error)
{
    auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    SSL_CTX* native = context->native_handle();
    // Nothing older than TLS 1.2: SSL 2 and 3, TLS 1.0 and 1.1 are out without naming them
    SSL_CTX_set_min_proto_version(native, TLS1_2_VERSION);
    context->set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::single_dh_use);
    if (!SSL_CTX_set_cipher_list(native, Tls12Ciphers))
    {
        error = "no usable cipher: " + LastOpenSslError();
        return nullptr;
    }

    boost::system::error_code ec;
    context->use_certificate_chain_file(certificateChainFile, ec);
    if (ec)
    {
        error = certificateChainFile + ": " + ec.message();
        return nullptr;
    }

    context->use_private_key_file(privateKeyFile, boost::asio::ssl::context::pem, ec);
    if (ec)
    {
        error = privateKeyFile + ": " + ec.message();
        return nullptr;
    }

    if (!SSL_CTX_check_private_key(native))
    {
        error = privateKeyFile + " is not the key of " + certificateChainFile;
        ERR_clear_error();
        return nullptr;
    }

    return context;
}
}
