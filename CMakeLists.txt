cmake_minimum_required(VERSION 3.20)
project(uia_agent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: /MT (Release) /MTd (Debug) -> exe has no VC++ runtime dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(uia_agent
  main.cpp
  uia.cpp
  win32.cpp
  capture.cpp
)

target_compile_options(uia_agent PRIVATE
  /utf-8
  /W4
  /WX-        # keep /W4 visible but not fatal
  /EHsc
  $<$<CONFIG:Release>:/O2>
  $<$<CONFIG:Debug>:/Od>
)

target_compile_definitions(uia_agent PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

# nlohmann single-header JSON, reused from the user's existing copy (D:\文档\C++\json.hpp).
target_include_directories(uia_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

target_link_libraries(uia_agent PRIVATE
  Uiautomationcore
  Ole32
  User32
  OleAut32
  Windowscodecs
  Gdi32
  Uuid
)