# Installs the daemon package and verifies an external public client consumer.
set(_install_prefix "${PHOTOSPIDER_DAEMON_BINARY_DIR}/consumer-install")
set(_consumer_binary "${PHOTOSPIDER_DAEMON_BINARY_DIR}/consumer-build")
set(_version_probe_root
    "${PHOTOSPIDER_DAEMON_BINARY_DIR}/package-version-probes")

file(
  REMOVE_RECURSE "${_install_prefix}" "${_consumer_binary}"
                 "${_version_probe_root}")

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

if(NOT DEFINED PHOTOSPIDER_DAEMON_INSTALL_BINDIR OR
   NOT DEFINED PHOTOSPIDER_DAEMON_INSTALL_LIBDIR OR
   NOT DEFINED PHOTOSPIDER_DAEMON_EXECUTABLE_SUFFIX)
  message(FATAL_ERROR "PhotospiderDaemon install layout was not provided")
endif()
if(IS_ABSOLUTE "${PHOTOSPIDER_DAEMON_INSTALL_BINDIR}")
  set(_installed_bindir "${PHOTOSPIDER_DAEMON_INSTALL_BINDIR}")
else()
  set(_installed_bindir
      "${_install_prefix}/${PHOTOSPIDER_DAEMON_INSTALL_BINDIR}")
endif()
if(IS_ABSOLUTE "${PHOTOSPIDER_DAEMON_INSTALL_LIBDIR}")
  set(_installed_libdir "${PHOTOSPIDER_DAEMON_INSTALL_LIBDIR}")
else()
  set(_installed_libdir
      "${_install_prefix}/${PHOTOSPIDER_DAEMON_INSTALL_LIBDIR}")
endif()

set(_targets_file
    "${_installed_libdir}/cmake/PhotospiderDaemon/PhotospiderDaemonTargets.cmake")
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
set(_installed_photospiderd
    "${_installed_bindir}/photospiderd${PHOTOSPIDER_DAEMON_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${_installed_photospiderd}")
  message(FATAL_ERROR "installed photospiderd runtime is missing")
endif()
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" -E env
    --unset=LD_LIBRARY_PATH
    --unset=DYLD_LIBRARY_PATH
    --unset=DYLD_FALLBACK_LIBRARY_PATH
    -- "${_installed_photospiderd}" --help
  RESULT_VARIABLE _photospiderd_help_result
  OUTPUT_VARIABLE _photospiderd_help_output
  ERROR_VARIABLE _photospiderd_help_error)
if(NOT _photospiderd_help_result EQUAL 0)
  message(
    FATAL_ERROR
      "installed photospiderd --help failed without loader environment:\n${_photospiderd_help_output}${_photospiderd_help_error}")
endif()
if(NOT "${_photospiderd_help_output}${_photospiderd_help_error}" MATCHES
   "Usage: photospiderd --socket PATH")
  message(FATAL_ERROR "installed photospiderd --help output is invalid")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}"
          ${_generator_args}
          -S "${PHOTOSPIDER_DAEMON_SOURCE_DIR}/tests/consumer"
          -B "${_consumer_binary}"
          "-DPhotospider_DIR:PATH=${PHOTOSPIDER_KERNEL_PACKAGE_DIR}"
          "-DPhotospiderDaemon_DIR:PATH=${_installed_libdir}/cmake/PhotospiderDaemon"
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

function(photospider_check_package_version package_name requested_version
         expected_result)
  string(TOLOWER "${package_name}" _package_slug)
  string(REPLACE "." "_" _version_slug "${requested_version}")
  set(_version_probe_binary
      "${_version_probe_root}/${_package_slug}-${_version_slug}-${expected_result}")
  execute_process(
    COMMAND
      "${CMAKE_COMMAND}"
      ${_generator_args}
      -S "${PHOTOSPIDER_DAEMON_SOURCE_DIR}/tests/version_probe"
      -B "${_version_probe_binary}"
      "-DPS_VERSION_PROBE_PACKAGE:STRING=${package_name}"
      "-DPS_VERSION_PROBE_VERSION:STRING=${requested_version}"
      "-DPS_VERSION_PROBE_EXPECT:STRING=${expected_result}"
      "-DPhotospider_DIR:PATH=${PHOTOSPIDER_KERNEL_PACKAGE_DIR}"
      "-DPhotospiderDaemon_DIR:PATH=${_installed_libdir}/cmake/PhotospiderDaemon"
      "-DCMAKE_BUILD_TYPE:STRING=${PHOTOSPIDER_BUILD_CONFIG}"
    RESULT_VARIABLE _version_probe_result
    OUTPUT_VARIABLE _version_probe_output
    ERROR_VARIABLE _version_probe_error)
  string(CONCAT _version_probe_diagnostic "${_version_probe_output}"
                                         "${_version_probe_error}")
  if(NOT _version_probe_result EQUAL 0)
    message(
      FATAL_ERROR
        "${package_name} ${requested_version} ${expected_result} probe failed:\n${_version_probe_diagnostic}"
    )
  endif()
endfunction()

foreach(_package_name IN ITEMS Photospider PhotospiderDaemon)
  photospider_check_package_version("${_package_name}" 0.2 compatible)
  photospider_check_package_version("${_package_name}" 0.1 incompatible)
  photospider_check_package_version("${_package_name}" 0.3 incompatible)
endforeach()
