cmake_minimum_required(VERSION 3.24)
project(steam2extract LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# The Steam2 decoder depends only on zlib plus AES crypto. We deliberately
# fetch the parser sources without enabling that project's GUI/FUSE targets.
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(zlib)

FetchContent_Declare(
    steam2_impl
    GIT_REPOSITORY https://github.com/dr3murr/steam2-winfsp.git
    GIT_TAG 2ad9e9d765aad349bfd3f1bd7e46533062bdb1f5
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(steam2_impl)
if(NOT steam2_impl_POPULATED)
    FetchContent_Populate(steam2_impl)
endif()

add_library(steam2core STATIC
    ${steam2_impl_SOURCE_DIR}/src/steam2_archive.cpp
    ${steam2_impl_SOURCE_DIR}/src/depot_keys.cpp
)
target_include_directories(steam2core PUBLIC ${steam2_impl_SOURCE_DIR}/include)
target_link_libraries(steam2core PUBLIC zlibstatic)

if(WIN32)
    target_link_libraries(steam2core PUBLIC bcrypt)
    target_compile_definitions(steam2core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    target_link_libraries(steam2core PUBLIC OpenSSL::Crypto)
endif()

add_executable(steam2extract main.cpp)
target_link_libraries(steam2extract PRIVATE steam2core)

if(MSVC)
    target_compile_options(steam2extract PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(steam2extract PRIVATE -Wall -Wextra -Wpedantic)
endif()
