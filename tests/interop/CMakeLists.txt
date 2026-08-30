cmake_minimum_required(VERSION 3.20)

project(PhotospiderDaemonInteropProbe LANGUAGES CXX)

set(PHOTOSPIDER_DAEMON_INTEROP_CLIENT "" CACHE STRING
  "Client package to use: old or new")
set_property(CACHE PHOTOSPIDER_DAEMON_INTEROP_CLIENT PROPERTY STRINGS old new)

if(PHOTOSPIDER_DAEMON_INTEROP_CLIENT STREQUAL "old")
  find_package(Photospider 0.1.0 REQUIRED COMPONENTS ipc_client)
  set(PHOTOSPIDER_DAEMON_INTEROP_TARGET
    Photospider::photospider_ipc_client)
elseif(PHOTOSPIDER_DAEMON_INTEROP_CLIENT STREQUAL "new")
  find_package(PhotospiderDaemon 0.1.0 REQUIRED)
  set(PHOTOSPIDER_DAEMON_INTEROP_TARGET PhotospiderDaemon::client)
else()
  message(FATAL_ERROR
    "PHOTOSPIDER_DAEMON_INTEROP_CLIENT must be old or new")
endif()

add_executable(ipc_compat_probe ipc_compat_probe.cpp)
target_compile_features(ipc_compat_probe PRIVATE cxx_std_17)
target_link_libraries(ipc_compat_probe PRIVATE
  ${PHOTOSPIDER_DAEMON_INTEROP_TARGET})
