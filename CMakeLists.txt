cmake_minimum_required(VERSION 3.16)
project(nexus VERSION 3.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

find_package(Threads REQUIRED)

add_library(sqlite3 STATIC third_party/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION)
target_link_libraries(sqlite3 PUBLIC ${CMAKE_DL_LIBS})

find_package(CURL QUIET)

file(GLOB SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
add_executable(nexus ${SOURCES})
target_include_directories(nexus PRIVATE include third_party)
target_link_libraries(nexus PRIVATE sqlite3 Threads::Threads)

if(CURL_FOUND)
    message(STATUS "Nexus Code: using libcurl HTTP backend")
    target_compile_definitions(nexus PRIVATE HAVE_LIBCURL=1)
    target_link_libraries(nexus PRIVATE CURL::libcurl)
else()
    message(STATUS "Nexus Code: libcurl dev headers not found, using curl(1) CLI backend")
endif()

install(TARGETS nexus RUNTIME DESTINATION bin)
