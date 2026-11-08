# OpenSSL 3: the EVP interface KCP and the hashes use, TLS 1.3
find_package(OpenSSL 3.2 REQUIRED)

add_library(openssl INTERFACE)

target_link_libraries(openssl
  INTERFACE
    OpenSSL::SSL
    OpenSSL::Crypto)
