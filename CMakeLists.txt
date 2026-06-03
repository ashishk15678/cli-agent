cmake_minimum_required(VERSION 3.13)

if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
  message(STATUS "Ignoring missing toolchain file: ${CMAKE_TOOLCHAIN_FILE}")
  unset(CMAKE_TOOLCHAIN_FILE CACHE)
endif()

project(codecrafters-claude-code LANGUAGES C)

file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS src/*.c)

set(CMAKE_C_STANDARD 23) # Enable the C23 standard
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(cJSON CONFIG REQUIRED)

add_executable(claude-code ${SOURCE_FILES})
target_include_directories(claude-code PRIVATE src)
target_compile_definitions(claude-code PRIVATE _GNU_SOURCE)
target_link_libraries(claude-code PRIVATE CURL::libcurl cjson)
