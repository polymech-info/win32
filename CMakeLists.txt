cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pm-win32
    VERSION 0.1.0
    DESCRIPTION "PolyMech Windows UI Automation and shell helpers"
    LANGUAGES CXX)
endif()

if(NOT WIN32)
  return()
endif()

if(NOT TARGET logger)
  include("${CMAKE_CURRENT_SOURCE_DIR}/../commons.cpp/cmake/logging.cmake")
endif()

# Clipboard and shell stay off this list. They include the host
# win/context/context_store.hpp and are compiled by tanit and tanit-cli.
add_library(pm-win32-lib STATIC
  src/inspect/app_inspect.cpp
  src/inspect/app_targets.cpp
  src/inspect/uia_spy.cpp)
target_include_directories(pm-win32-lib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(pm-win32-lib PUBLIC logger)
target_link_libraries(pm-win32-lib PRIVATE
  uiautomationcore.lib
  windowscodecs.lib
  ole32
  oleaut32
  user32
  gdi32)
target_compile_features(pm-win32-lib PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(pm-win32-lib PRIVATE /W3 /utf-8 /EHsc)
  target_compile_definitions(pm-win32-lib PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    _WIN32_WINNT=0x0A00)
endif()
