cmake_minimum_required(VERSION 3.20)

project(PhotospiderDaemon VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND
   NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "PhotospiderDaemon supports only Darwin and Linux Unix-domain IPC")
endif()

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
    "Choose the build type" FORCE)
endif()

set(PHOTOSPIDER_DAEMON_DEPENDENCY_LIBDIR "" CACHE PATH
  "Installed Photospider runtime directory added to photospiderd INSTALL_RPATH")
set(PHOTOSPIDER_DAEMON_INSTALL_CMAKEDIR
  "${CMAKE_INSTALL_LIBDIR}/cmake/PhotospiderDaemon" CACHE STRING
  "Install directory for the PhotospiderDaemon CMake package")

find_package(Threads REQUIRED)
find_package(Photospider 0.1.0 REQUIRED COMPONENTS
  embedded
  operation_runtime
  operation_plugin_sdk
  policy_sdk)
find_package(nlohmann_json 3.9 REQUIRED)

foreach(REQUIRED_TARGET
    Photospider::photospider
    Photospider::operation_runtime
    Photospider::operation_plugin_sdk
    Photospider::policy_sdk)
  if(NOT TARGET ${REQUIRED_TARGET})
    message(FATAL_ERROR
      "Installed Photospider package omitted required target ${REQUIRED_TARGET}")
  endif()
endforeach()

set(PHOTOSPIDER_DAEMON_PUBLIC_HEADERS
  include/photospider/ipc/client.hpp
  include/photospider/ipc/host.hpp
  include/photospider/ipc/protocol.hpp)

configure_file(
  "${PROJECT_SOURCE_DIR}/src/lib/ipc/service_version.hpp.in"
  "${CMAKE_BINARY_DIR}/generated/ipc/service_version.hpp"
  @ONLY)

add_library(photospider_daemon_client_objects OBJECT
  src/lib/ipc/frame.cpp
  src/lib/ipc/unix_socket.cpp
  src/lib/ipc/codec.cpp
  src/lib/ipc/client.cpp
  src/lib/ipc/host.cpp)
target_compile_features(photospider_daemon_client_objects PRIVATE cxx_std_17)
target_include_directories(photospider_daemon_client_objects PRIVATE
  "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/src/lib")
target_link_libraries(photospider_daemon_client_objects PRIVATE
  Photospider::operation_runtime
  nlohmann_json::nlohmann_json
  Threads::Threads)

add_library(photospider_daemon_client STATIC
  $<TARGET_OBJECTS:photospider_daemon_client_objects>)
add_library(PhotospiderDaemon::client ALIAS photospider_daemon_client)
set_target_properties(photospider_daemon_client PROPERTIES
  EXPORT_NAME client
  OUTPUT_NAME photospider_daemon_client)
target_compile_definitions(photospider_daemon_client PUBLIC PHOTOSPIDER_STATIC)
target_compile_features(photospider_daemon_client PUBLIC cxx_std_17)
target_include_directories(photospider_daemon_client PUBLIC
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(photospider_daemon_client PUBLIC
  Photospider::operation_runtime
  Threads::Threads)

add_library(photospider_daemon_server_internal STATIC
  src/lib/ipc/collection_snapshot_registry.cpp
  src/lib/ipc/compute_request_registry.cpp
  src/lib/ipc/output_store.cpp
  src/lib/ipc/session_registry.cpp
  src/lib/ipc/request_router.cpp
  src/lib/ipc/server.cpp)
target_compile_features(photospider_daemon_server_internal PRIVATE cxx_std_17)
target_include_directories(photospider_daemon_server_internal PRIVATE
  "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/src/lib")
target_link_libraries(photospider_daemon_server_internal PRIVATE
  photospider_daemon_client
  nlohmann_json::nlohmann_json
  Threads::Threads)

add_executable(photospiderd apps/photospiderd/main.cpp)
target_compile_features(photospiderd PRIVATE cxx_std_17)
target_include_directories(photospiderd PRIVATE
  "${PROJECT_SOURCE_DIR}/include"
  "${PROJECT_SOURCE_DIR}/src/lib"
  "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(photospiderd PRIVATE
  photospider_daemon_server_internal
  photospider_daemon_client
  Photospider::photospider
  Threads::Threads)

set(PHOTOSPIDERD_INSTALL_RPATH "")
if(NOT IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
  file(RELATIVE_PATH PHOTOSPIDERD_RELATIVE_LIBDIR
    "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBDIR}")
  if(PHOTOSPIDERD_RELATIVE_LIBDIR STREQUAL "")
    set(PHOTOSPIDERD_RELATIVE_LIBDIR ".")
  endif()
  if(APPLE)
    list(APPEND PHOTOSPIDERD_INSTALL_RPATH
      "@loader_path/${PHOTOSPIDERD_RELATIVE_LIBDIR}")
  else()
    list(APPEND PHOTOSPIDERD_INSTALL_RPATH
      "$ORIGIN/${PHOTOSPIDERD_RELATIVE_LIBDIR}")
  endif()
else()
  list(APPEND PHOTOSPIDERD_INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")
endif()
if(PHOTOSPIDER_DAEMON_DEPENDENCY_LIBDIR)
  list(APPEND PHOTOSPIDERD_INSTALL_RPATH
    "${PHOTOSPIDER_DAEMON_DEPENDENCY_LIBDIR}")
endif()
set_target_properties(photospiderd PROPERTIES
  BUILD_RPATH_USE_ORIGIN ON
  INSTALL_RPATH "${PHOTOSPIDERD_INSTALL_RPATH}")
if(APPLE)
  set_target_properties(photospiderd PROPERTIES MACOSX_RPATH ON)
endif()

install(TARGETS photospider_daemon_client
  EXPORT PhotospiderDaemonTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS photospiderd
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
foreach(PUBLIC_HEADER IN LISTS PHOTOSPIDER_DAEMON_PUBLIC_HEADERS)
  file(RELATIVE_PATH PUBLIC_HEADER_RELATIVE
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/${PUBLIC_HEADER}")
  get_filename_component(PUBLIC_HEADER_DIRECTORY
    "${PUBLIC_HEADER_RELATIVE}" DIRECTORY)
  install(FILES "${PROJECT_SOURCE_DIR}/${PUBLIC_HEADER}"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${PUBLIC_HEADER_DIRECTORY}")
endforeach()
install(FILES
  docs/codebase-structure/IPC-Protocol-v2.md
  docs/codebase-structure/zh/IPC-Protocol-v2.zh.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/photospider-daemon")

configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/PhotospiderDaemonConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/PhotospiderDaemonConfig.cmake"
  INSTALL_DESTINATION "${PHOTOSPIDER_DAEMON_INSTALL_CMAKEDIR}")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/PhotospiderDaemonConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion)
install(EXPORT PhotospiderDaemonTargets
  FILE PhotospiderDaemonTargets.cmake
  NAMESPACE PhotospiderDaemon::
  DESTINATION "${PHOTOSPIDER_DAEMON_INSTALL_CMAKEDIR}")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/PhotospiderDaemonConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/PhotospiderDaemonConfigVersion.cmake"
  DESTINATION "${PHOTOSPIDER_DAEMON_INSTALL_CMAKEDIR}")

if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  include(GoogleTest)

  set(PHOTOSPIDER_DAEMON_TEST_RUNTIME_DIR "${CMAKE_BINARY_DIR}/tests")
  set(PHOTOSPIDER_DAEMON_TEST_PLUGIN_DIR "${CMAKE_BINARY_DIR}/test_plugins")
  set(PHOTOSPIDER_DAEMON_TEST_POLICY_DIR "${CMAKE_BINARY_DIR}/test_policies")

  add_library(test_policy_fixture_state SHARED
    tests/fixtures/policy_plugins/policy_fixture_state.c)
  set_target_properties(test_policy_fixture_state PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    LIBRARY_OUTPUT_DIRECTORY "${PHOTOSPIDER_DAEMON_TEST_RUNTIME_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${PHOTOSPIDER_DAEMON_TEST_RUNTIME_DIR}")
  target_link_libraries(test_policy_fixture_state PRIVATE
    Photospider::policy_sdk)

  add_library(test_policy_plugin SHARED
    tests/fixtures/policy_plugins/policy_fixture.c)
  set_target_properties(test_policy_plugin PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    LIBRARY_OUTPUT_DIRECTORY "${PHOTOSPIDER_DAEMON_TEST_POLICY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${PHOTOSPIDER_DAEMON_TEST_POLICY_DIR}"
    BUILD_RPATH "${PHOTOSPIDER_DAEMON_TEST_RUNTIME_DIR}")
  target_link_libraries(test_policy_plugin PRIVATE
    Photospider::policy_sdk
    test_policy_fixture_state)

  add_library(lifecycle_op_plugin SHARED
    tests/fixtures/ops/lifecycle_op_plugin.cpp)
  target_compile_definitions(lifecycle_op_plugin PRIVATE
    PHOTOSPIDER_PLUGIN_BUILD)
  target_compile_features(lifecycle_op_plugin PRIVATE cxx_std_17)
  set_target_properties(lifecycle_op_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_DIR}/lifecycle"
    RUNTIME_OUTPUT_DIRECTORY "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_DIR}/lifecycle")
  target_link_libraries(lifecycle_op_plugin PRIVATE
    Photospider::operation_plugin_sdk)

  add_executable(photospider_daemon_test_manifest_signer
    tests/support/plugin_trust_manifest_signer.cpp)
  target_compile_features(photospider_daemon_test_manifest_signer PRIVATE
    cxx_std_17)
  target_link_libraries(photospider_daemon_test_manifest_signer PRIVATE
    OpenSSL::Crypto)

  set(PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_DIR
    "${CMAKE_BINARY_DIR}/generated/plugin_trust")
  set(PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_MANIFEST
    "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_DIR}/manifest.txt")
  set(PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_SIGNATURE
    "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_DIR}/signature.hex")
  set(PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_BAD_SIGNATURE
    "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_DIR}/bad-signature.hex")
  set(PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_ENVIRONMENT
    "PHOTOSPIDER_PLUGIN_TRUST_MANIFEST=${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_MANIFEST}"
    "PHOTOSPIDER_PLUGIN_TRUST_SIGNATURE=${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_SIGNATURE}"
    "PHOTOSPIDER_PLUGIN_TRUST_PUBLIC_KEY=${PROJECT_SOURCE_DIR}/tests/fixtures/trust/test_ed25519_public_key.pem")

  # Generate a real signed trust tuple for the standalone repository's two
  # process-native plugin fixtures. The private key is maintained test data and
  # neither it nor the generated bundle is part of the installed package.
  add_custom_target(photospider_daemon_test_plugin_trust_bundle
    COMMAND "${Python3_EXECUTABLE}" -B
      "${PROJECT_SOURCE_DIR}/tests/support/generate_plugin_trust_bundle.py"
      --signer "$<TARGET_FILE:photospider_daemon_test_manifest_signer>"
      --private-seed
        "${PROJECT_SOURCE_DIR}/tests/fixtures/trust/test_ed25519_private_seed.hex"
      --manifest "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_MANIFEST}"
      --signature "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_SIGNATURE}"
      --bad-signature
        "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_BAD_SIGNATURE}"
      --trust-root photospider-maintained-test-root
      --entry operation 11111111111111111111111111111111 1
        "$<TARGET_FILE:lifecycle_op_plugin>"
      --entry policy 22222222222222222222222222222222 1
        "$<TARGET_FILE:test_policy_plugin>"
    BYPRODUCTS
      "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_MANIFEST}"
      "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_SIGNATURE}"
      "${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_BAD_SIGNATURE}"
    DEPENDS
      "${PROJECT_SOURCE_DIR}/tests/support/generate_plugin_trust_bundle.py"
      "${PROJECT_SOURCE_DIR}/tests/fixtures/trust/test_ed25519_private_seed.hex"
      photospider_daemon_test_manifest_signer
      lifecycle_op_plugin
      test_policy_plugin
    VERBATIM)

  # GoogleTest discovery must observe the same immutable trust tuple as the
  # registered test commands. TEST_LAUNCHER is native from CMake 3.29 onward;
  # CROSSCOMPILING_EMULATOR is its portable pre-3.29 launcher hook.
  function(photospider_daemon_enable_plugin_trust_test_environment TARGET_NAME)
    set(PLUGIN_TRUST_TEST_LAUNCHER
      "${CMAKE_COMMAND}"
      -E
      env
      ${PHOTOSPIDER_DAEMON_TEST_PLUGIN_TRUST_ENVIRONMENT})
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
      set_property(TARGET ${TARGET_NAME} PROPERTY TEST_LAUNCHER
        ${PLUGIN_TRUST_TEST_LAUNCHER})
    else()
      set_property(TARGET ${TARGET_NAME} PROPERTY CROSSCOMPILING_EMULATOR
        ${PLUGIN_TRUST_TEST_LAUNCHER})
    endif()
  endfunction()

  function(add_photospider_daemon_test TARGET_NAME SOURCE_PATH)
    cmake_parse_arguments(TEST "PLUGIN_TRUST" "" "" ${ARGN})
    add_executable(${TARGET_NAME} "${SOURCE_PATH}")
    set_target_properties(${TARGET_NAME} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PHOTOSPIDER_DAEMON_TEST_RUNTIME_DIR}")
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_17)
    target_include_directories(${TARGET_NAME} PRIVATE
      "${PROJECT_SOURCE_DIR}/include"
      "${PROJECT_SOURCE_DIR}/src/lib"
      "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(${TARGET_NAME} PRIVATE
      photospider_daemon_server_internal
      photospider_daemon_client
      Photospider::photospider
      GTest::gtest
      GTest::gtest_main
      nlohmann_json::nlohmann_json
      Threads::Threads
      ${CMAKE_DL_LIBS})
    if(TEST_PLUGIN_TRUST)
      photospider_daemon_enable_plugin_trust_test_environment(${TARGET_NAME})
      add_dependencies(${TARGET_NAME}
        photospider_daemon_test_plugin_trust_bundle)
    endif()
    gtest_discover_tests(${TARGET_NAME}
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      DISCOVERY_TIMEOUT 30
      PROPERTIES TIMEOUT 30)
  endfunction()

  add_photospider_daemon_test(test_ipc_protocol
    tests/unit/test_ipc_protocol.cpp
    PLUGIN_TRUST)
  target_compile_definitions(test_ipc_protocol PRIVATE
    PS_TEST_POLICY_PLUGIN_PATH="$<TARGET_FILE:test_policy_plugin>"
    PS_TEST_OP_PLUGIN_DIR="$<TARGET_FILE_DIR:lifecycle_op_plugin>")
  add_dependencies(test_ipc_protocol test_policy_plugin lifecycle_op_plugin)

  add_photospider_daemon_test(test_ipc_host
    tests/unit/test_ipc_host.cpp)
  add_photospider_daemon_test(test_compute_request_registry
    tests/unit/test_compute_request_registry.cpp)
  add_photospider_daemon_test(test_collection_snapshot_registry
    tests/unit/test_collection_snapshot_registry.cpp)
  add_photospider_daemon_test(test_output_store
    tests/unit/test_output_store.cpp)

  add_executable(ipc_output_fixture_daemon
    tests/support/ipc_output_fixture_daemon.cpp)
  set_target_properties(ipc_output_fixture_daemon PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PHOTOSPIDER_DAEMON_TEST_RUNTIME_DIR}")
  target_compile_features(ipc_output_fixture_daemon PRIVATE cxx_std_17)
  target_include_directories(ipc_output_fixture_daemon PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/lib"
    "${PROJECT_SOURCE_DIR}/tests")
  target_link_libraries(ipc_output_fixture_daemon PRIVATE
    photospider_daemon_server_internal
    photospider_daemon_client
    Photospider::photospider
    Threads::Threads)

  add_photospider_daemon_test(test_ipc_daemon
    tests/integration/test_ipc_daemon.cpp
    PLUGIN_TRUST)
  target_compile_definitions(test_ipc_daemon PRIVATE
    PS_PHOTOSPIDERD_PATH="$<TARGET_FILE:photospiderd>"
    PS_IPC_OUTPUT_FIXTURE_DAEMON_PATH="$<TARGET_FILE:ipc_output_fixture_daemon>"
    PS_TEST_OP_PLUGIN_DIR="$<TARGET_FILE_DIR:lifecycle_op_plugin>"
    PS_TEST_POLICY_PLUGIN_PATH="$<TARGET_FILE:test_policy_plugin>")
  add_dependencies(test_ipc_daemon photospiderd ipc_output_fixture_daemon
    lifecycle_op_plugin test_policy_plugin)

  add_test(NAME PhotospiderdCapabilityHelp
    COMMAND "${CMAKE_COMMAND}"
      -DPHOTOSPIDERD=$<TARGET_FILE:photospiderd>
      -P "${PROJECT_SOURCE_DIR}/tests/integration/photospiderd_capability_help.cmake")
  set_tests_properties(PhotospiderdCapabilityHelp PROPERTIES TIMEOUT 10)

  add_test(NAME PhotospiderDaemonInstalledConsumer
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/integration/installed_consumer_smoke.py"
      --source "${PROJECT_SOURCE_DIR}"
      --producer-build "${CMAKE_BINARY_DIR}"
      --work "${CMAKE_BINARY_DIR}/installed-consumer-smoke"
      --cmake "${CMAKE_COMMAND}"
      --generator "${CMAKE_GENERATOR}"
      --config "$<CONFIG>")
  set_tests_properties(PhotospiderDaemonInstalledConsumer PROPERTIES
    TIMEOUT 180
    ENVIRONMENT
      "PHOTOSPIDER_DAEMON_TEST_PHOTOSPIDER_DIR=${Photospider_DIR}")

  add_test(NAME PhotospiderdInstallLayoutSmoke
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/tests/integration/photospiderd_install_layout_smoke.py"
      --repo "${PROJECT_SOURCE_DIR}"
      --photospider-dir "${Photospider_DIR}"
      --dependency-libdir "${PHOTOSPIDER_DAEMON_DEPENDENCY_LIBDIR}"
      --build-root "${CMAKE_BINARY_DIR}"
      --work "${CMAKE_BINARY_DIR}/install-layout-smoke"
      --cmake-executable "${CMAKE_COMMAND}"
      --generator "${CMAKE_GENERATOR}"
      --config "$<CONFIG>"
      --osx-architectures "${CMAKE_OSX_ARCHITECTURES}")
  set_tests_properties(PhotospiderdInstallLayoutSmoke PROPERTIES TIMEOUT 600)

  # Manual four-cell migration gate probe. The executable itself is a durable
  # public-boundary smoke, but the old/new orchestration is intentionally not
  # registered with CTest because it is frozen-baseline migration evidence.
  add_executable(ipc_compat_probe tests/interop/ipc_compat_probe.cpp)
  target_compile_features(ipc_compat_probe PRIVATE cxx_std_17)
  target_link_libraries(ipc_compat_probe PRIVATE
    PhotospiderDaemon::client)
endif()

message(STATUS
  "PhotospiderDaemon v${PROJECT_VERSION}: IPC v2 client + photospiderd")
