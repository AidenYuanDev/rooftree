# libcurl, for rooftree-server's SMTP mailer
find_package(CURL 7.86 REQUIRED)

add_library(curl INTERFACE)

target_link_libraries(curl
  INTERFACE
    CURL::libcurl)
