/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The TLS context of a server
 * facing browsers, from the PEM files certbot writes. TLS 1.2 and 1.3 only, forward secret AEAD
 * ciphers (Mozilla's "intermediate"), so any browser of the last years connects and nothing weaker is
 * offered.
 *
 * A context is never changed once built: a renewed certificate is a new context, new connections get
 * it and the open ones keep theirs (each holds its shared_ptr).
 */

#ifndef ROOFTREE_SSL_CONTEXT_H
#define ROOFTREE_SSL_CONTEXT_H

#include "Define.h"
#include <boost/asio/ssl/context.hpp>
#include <memory>
#include <string>

namespace Rooftree::Net
{
// certificateChainFile: the certificate followed by the intermediates (certbot's fullchain.pem).
// Null, with error set, if a file can't be read or the key doesn't belong to the certificate
std::shared_ptr<boost::asio::ssl::context> CreateServerSslContext(std::string const& certificateChainFile,
    std::string const& privateKeyFile, std::string& error);
}

#endif // ROOFTREE_SSL_CONTEXT_H
