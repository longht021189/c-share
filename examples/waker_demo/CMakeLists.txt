add_executable(waker_demo main.cpp)

target_link_libraries(waker_demo PRIVATE c_share::waker)

if(WIN32)
    target_link_libraries(waker_demo PRIVATE ws2_32)
endif()

c_share_set_compile_options(waker_demo)
