find_package(CURL REQUIRED)
# permessage-deflate (src/ws_codec.cpp). Already a dependency of libcurl.
find_package(ZLIB REQUIRED)

add_library(http STATIC
    src/client.cpp
    src/client_c.cpp
    src/ws_codec.cpp
)
add_library(c_share::http ALIAS http)

target_include_directories(http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# libcurl is strictly an implementation detail (see client.hpp) -- not
# reflected in this module's public header, so it's a PRIVATE link, not
# PUBLIC: consumers of c_share::http never need <curl/curl.h> themselves.
target_link_libraries(http
    PRIVATE
        CURL::libcurl
        ZLIB::ZLIB
        c_share::waker
)

c_share_set_compile_options(http)
