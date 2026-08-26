cmake_minimum_required(VERSION 3.20)
project(x4llm)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows (MinGW cross-build): statically link the MinGW runtime so the DLL only
# depends on Windows system libs (KERNEL32, UCRT, WS2_32). Must be set before
# add_library() so it lands at the start of the shared link command.
if(WIN32)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static")
endif()

add_library(x4llm SHARED x4llm.cpp)

target_include_directories(x4llm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk
    ${CMAKE_CURRENT_SOURCE_DIR}/../../sdk/sdk
)

if(WIN32)
    # WinSock2 for the raw-socket HTTP call (see x4llm.cpp).
    target_link_libraries(x4llm PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(x4llm PRIVATE Threads::Threads)
endif()

set_target_properties(x4llm PROPERTIES
    PREFIX ""
    OUTPUT_NAME "x4llm"
)
