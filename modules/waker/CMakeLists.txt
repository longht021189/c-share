if(WIN32)
    set(C_SHARE_WAKER_BACKEND src/waker_win.cpp)
elseif(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    set(C_SHARE_WAKER_BACKEND src/waker_kqueue.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(C_SHARE_WAKER_BACKEND src/waker_epoll.cpp)
else()
    message(FATAL_ERROR "c_share::waker has no backend for platform '${CMAKE_SYSTEM_NAME}'")
endif()

add_library(waker STATIC
    ${C_SHARE_WAKER_BACKEND}
)
add_library(c_share::waker ALIAS waker)

target_include_directories(waker
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(WIN32)
    target_link_libraries(waker PRIVATE ws2_32)
endif()

c_share_set_compile_options(waker)
