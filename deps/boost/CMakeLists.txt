find_package(Boost 1.83
  REQUIRED
  COMPONENTS
    filesystem
    json
    url
  CONFIG)

add_library(boost INTERFACE)

target_link_libraries(boost
  INTERFACE
    Boost::headers
    Boost::filesystem)

target_compile_definitions(boost
  INTERFACE
    BOOST_CONFIG_SUPPRESS_OUTDATED_MESSAGE
    BOOST_ASIO_NO_DEPRECATED
    BOOST_BIND_NO_PLACEHOLDERS
    BOOST_SYSTEM_USE_UTF8)

# For code that speaks JSON: rooftree-server and apps/, the base libraries don't use it
add_library(boost-json INTERFACE)

target_link_libraries(boost-json
  INTERFACE
    boost
    Boost::json)

# Path, query and form decoding of rooftree-network's HTTP requests
add_library(boost-url INTERFACE)

target_link_libraries(boost-url
  INTERFACE
    boost
    Boost::url)
