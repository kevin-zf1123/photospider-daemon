cmake_minimum_required(VERSION 3.21)
project(PhotospiderDaemonConsumer LANGUAGES CXX)

find_package(PhotospiderDaemon 0.2 CONFIG REQUIRED COMPONENTS client)

add_executable(photospider_daemon_consumer main.cpp)
target_compile_features(photospider_daemon_consumer PRIVATE cxx_std_17)
target_link_libraries(
  photospider_daemon_consumer PRIVATE PhotospiderDaemon::client)
