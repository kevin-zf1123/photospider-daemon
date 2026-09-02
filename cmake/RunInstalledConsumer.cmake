# Installs the daemon package and verifies an external public client consumer.
set(_install_prefix "${PHOTOSPIDER_DAEMON_BINARY_DIR}/consumer-install")
set(_consumer_binary "${PHOTOSPIDER_DAEMON_BINARY_DIR}/consumer-build")

file(REMOVE_RECURSE "${_install_prefix}" "${_consumer_binary}")

set(_config_args)
if(PHOTOSPIDER_BUILD_CONFIG)
  list(APPEND _config_args --config "${PHOTOSPIDER_BUILD_CONFIG}")
endif()

if(NOT PHOTOSPIDER_GENERATOR)
  message(FATAL_ERROR "PhotospiderDaemon consumer generator was not provided")
endif()
set(_generator_args -G "${PHOTOSPIDER_GENERATOR}")
if(PHOTOSPIDER_GENERATOR_PLATFORM)
  list(APPEND _generator_args -A "${PHOTOSPIDER_GENERATOR_PLATFORM}")
endif()
if(PHOTOSPIDER_GENERATOR_TOOLSET)
  list(APPEND _generator_args -T "${PHOTOSPIDER_GENERATOR_TOOLSET}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${PHOTOSPIDER_DAEMON_BINARY_DIR}"
          --prefix "${_install_prefix}" ${_config_args}
  RESULT_VARIABLE _install_result)
if(NOT _install_result EQUAL 0)
  message(FATAL_ERROR "PhotospiderDaemon isolated install failed")
endif()

set(_targets_file
    "${_install_prefix}/lib/cmake/PhotospiderDaemon/PhotospiderDaemonTargets.cmake")
if(NOT EXISTS "${_targets_file}")
  message(FATAL_ERROR "PhotospiderDaemon exported target file is missing")
endif()
file(READ "${_targets_file}" _targets_content)
if(NOT _targets_content MATCHES "PhotospiderDaemon::client")
  message(FATAL_ERROR "PhotospiderDaemon::client is not exported")
endif()
if(_targets_content MATCHES "PhotospiderDaemon::photospiderd")
  message(FATAL_ERROR "photospiderd executable leaked into package exports")
endif()
if(NOT EXISTS "${_install_prefix}/bin/photospiderd")
  message(FATAL_ERROR "installed photospiderd runtime is missing")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
          ${_generator_args}
          -S "${PHOTOSPIDER_DAEMON_SOURCE_DIR}/tests/consumer"
          -B "${_consumer_binary}"
          "-DPhotospider_DIR:PATH=${PHOTOSPIDER_KERNEL_PACKAGE_DIR}"
          "-DPhotospiderDaemon_DIR:PATH=${_install_prefix}/lib/cmake/PhotospiderDaemon"
          "-DCMAKE_BUILD_TYPE:STRING=${PHOTOSPIDER_BUILD_CONFIG}"
  RESULT_VARIABLE _configure_result)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR "PhotospiderDaemon consumer configure failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${_consumer_binary}"
          --target run_photospider_daemon_consumer ${_config_args}
  RESULT_VARIABLE _build_result)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "PhotospiderDaemon consumer build or runtime failed")
endif()
