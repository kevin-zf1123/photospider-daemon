cmake_minimum_required(VERSION 3.21)

project(
  PhotospiderDaemon
  VERSION 0.2.0
  DESCRIPTION "Same-user non-persistent local Photospider orchestration"
  LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

option(PHOTOSPIDER_DAEMON_ENABLE_ASAN
       "Enable AddressSanitizer for daemon validation" OFF)
option(PHOTOSPIDER_DAEMON_ENABLE_TSAN
       "Enable ThreadSanitizer for daemon validation" OFF)
option(
  PHOTOSPIDER_DAEMON_BUILD_MANUAL_FUZZ_TARGETS
  "Build explicit long-lived libFuzzer harnesses outside default build/CTest"
  OFF)

if(PHOTOSPIDER_DAEMON_ENABLE_ASAN AND PHOTOSPIDER_DAEMON_ENABLE_TSAN)
  message(FATAL_ERROR "daemon ASAN and TSAN cannot be enabled together")
endif()

find_package(Photospider 0.2 CONFIG REQUIRED COMPONENTS kernel)
find_package(Threads REQUIRED)

add_library(
  photospider_daemon_client
  STATIC
  src/lib/ipc/client.cpp
  src/lib/ipc/codec.cpp
  src/lib/ipc/frame.cpp
  src/lib/ipc/unix_socket.cpp
  src/lib/orchestration/service.cpp
  src/lib/server/server.cpp)
add_library(PhotospiderDaemon::client ALIAS photospider_daemon_client)
set_target_properties(
  photospider_daemon_client
  PROPERTIES CXX_EXTENSIONS OFF
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN YES
             EXPORT_NAME client)
target_compile_features(photospider_daemon_client PUBLIC cxx_std_17)
target_include_directories(
  photospider_daemon_client
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/lib)
target_link_libraries(
  photospider_daemon_client
  PUBLIC Photospider::kernel
  PRIVATE Threads::Threads)
target_compile_definitions(
  photospider_daemon_client
  PRIVATE PHOTOSPIDER_DAEMON_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(
    photospider_daemon_client PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(PHOTOSPIDER_DAEMON_ENABLE_ASAN)
    target_compile_options(
      photospider_daemon_client PUBLIC -fsanitize=address
                                       -fno-omit-frame-pointer)
    target_link_options(photospider_daemon_client PUBLIC -fsanitize=address)
  elseif(PHOTOSPIDER_DAEMON_ENABLE_TSAN)
    target_compile_options(
      photospider_daemon_client PUBLIC -fsanitize=thread
                                       -fno-omit-frame-pointer)
    target_link_options(photospider_daemon_client PUBLIC -fsanitize=thread)
  endif()
endif()

add_executable(photospiderd apps/photospiderd/main.cpp)
target_compile_features(photospiderd PRIVATE cxx_std_17)
target_include_directories(photospiderd PRIVATE src/lib)
target_link_libraries(photospiderd PRIVATE photospider_daemon_client)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(photospiderd PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(PHOTOSPIDER_DAEMON_BUILD_MANUAL_FUZZ_TARGETS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "daemon manual fuzz target requires Clang libFuzzer")
  endif()
  if(PHOTOSPIDER_DAEMON_ENABLE_TSAN)
    message(FATAL_ERROR "daemon libFuzzer target cannot combine with TSAN")
  endif()
  target_compile_options(
    photospider_daemon_client
    PRIVATE -fsanitize=fuzzer-no-link -fno-omit-frame-pointer)
  target_link_options(photospider_daemon_client
                      INTERFACE -fsanitize=fuzzer-no-link)
  add_executable(
    photospider_daemon_frame_codec_fuzz EXCLUDE_FROM_ALL
    tests/fuzz/frame_codec_fuzz.cpp)
  target_include_directories(
    photospider_daemon_frame_codec_fuzz PRIVATE src/lib)
  target_link_libraries(
    photospider_daemon_frame_codec_fuzz PRIVATE photospider_daemon_client)
  target_compile_features(photospider_daemon_frame_codec_fuzz
                          PRIVATE cxx_std_17)
  target_compile_options(
    photospider_daemon_frame_codec_fuzz
    PRIVATE -fsanitize=fuzzer -fno-omit-frame-pointer -Wall -Wextra -Wpedantic
            -Werror)
  target_link_options(photospider_daemon_frame_codec_fuzz
                      PRIVATE -fsanitize=fuzzer)
endif()

install(
  TARGETS photospider_daemon_client
  EXPORT PhotospiderDaemonTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS photospiderd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/photospider DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  EXPORT PhotospiderDaemonTargets
  FILE PhotospiderDaemonTargets.cmake
  NAMESPACE PhotospiderDaemon::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PhotospiderDaemon)

configure_package_config_file(
  cmake/PhotospiderDaemonConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/PhotospiderDaemonConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PhotospiderDaemon)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/PhotospiderDaemonConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion)
install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/PhotospiderDaemonConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/PhotospiderDaemonConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PhotospiderDaemon)

if(BUILD_TESTING)
  add_executable(test_ipc_codec tests/unit/test_ipc_codec.cpp)
  target_compile_features(test_ipc_codec PRIVATE cxx_std_17)
  target_include_directories(test_ipc_codec PRIVATE src/lib tests)
  target_link_libraries(test_ipc_codec PRIVATE photospider_daemon_client)

  add_executable(test_local_daemon tests/integration/test_local_daemon.cpp)
  target_compile_features(test_local_daemon PRIVATE cxx_std_17)
  target_include_directories(test_local_daemon PRIVATE src/lib tests)
  target_link_libraries(test_local_daemon PRIVATE photospider_daemon_client)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(test_ipc_codec PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(test_local_daemon PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  add_test(NAME test_ipc_codec COMMAND test_ipc_codec)
  add_test(NAME test_local_daemon COMMAND test_local_daemon)
  add_test(NAME test_photospiderd_help COMMAND photospiderd --help)
  set_tests_properties(
    test_photospiderd_help
    PROPERTIES PASS_REGULAR_EXPRESSION
               "Usage: photospiderd --socket PATH")

  add_test(
    NAME test_installed_client_consumer
    COMMAND
      ${CMAKE_COMMAND}
      -DPHOTOSPIDER_DAEMON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DPHOTOSPIDER_DAEMON_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DPHOTOSPIDER_KERNEL_PACKAGE_DIR=${Photospider_DIR}
      -DPHOTOSPIDER_BUILD_CONFIG=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunInstalledConsumer.cmake)
endif()
