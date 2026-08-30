cmake_minimum_required(VERSION 3.24)
project(steam2tool VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

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

# Import only the Steam2 decoder sources we use. Do not call
# FetchContent_MakeAvailable() because the upstream project also defines
# optional FUSE/WinFsp targets that are not needed by this tool.
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

add_executable(steam2tool steam2tool.cpp)
target_link_libraries(steam2tool PRIVATE steam2core)

# Cross-platform native GUI. This is C++/FLTK only; no Python or Tkinter.
set(FLTK_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FORMS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_GL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    fltk
    GIT_REPOSITORY https://github.com/fltk/fltk.git
    GIT_TAG release-1.4.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fltk)

if(WIN32)
    add_executable(steam2tool-gui WIN32 steam2tool_gui.cpp)
else()
    add_executable(steam2tool-gui steam2tool_gui.cpp)
endif()
target_link_libraries(steam2tool-gui PRIVATE fltk::fltk)

if(MSVC)
    target_compile_options(steam2tool PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(steam2tool-gui PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(steam2tool PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(steam2tool-gui PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS steam2tool steam2tool-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
