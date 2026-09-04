cmake_minimum_required(VERSION 3.21)
project(PhotospiderVersionProbe LANGUAGES CXX)

if(NOT PS_VERSION_PROBE_VERSION OR
   NOT PS_VERSION_PROBE_EXPECT MATCHES "^(compatible|incompatible)$")
  message(FATAL_ERROR "Version probe requires a version and expectation")
endif()

if(PS_VERSION_PROBE_PACKAGE STREQUAL "Photospider")
  find_package(Photospider ${PS_VERSION_PROBE_VERSION} CONFIG QUIET
               COMPONENTS kernel)
  set(_probe_found "${Photospider_FOUND}")
  set(_probe_version "${Photospider_VERSION}")
  set(_probe_versions "${Photospider_CONSIDERED_VERSIONS}")
elseif(PS_VERSION_PROBE_PACKAGE STREQUAL "PhotospiderDaemon")
  find_package(PhotospiderDaemon ${PS_VERSION_PROBE_VERSION} CONFIG QUIET
               COMPONENTS client)
  set(_probe_found "${PhotospiderDaemon_FOUND}")
  set(_probe_version "${PhotospiderDaemon_VERSION}")
  set(_probe_versions "${PhotospiderDaemon_CONSIDERED_VERSIONS}")
else()
  message(FATAL_ERROR "Unknown package version probe")
endif()

if(PS_VERSION_PROBE_EXPECT STREQUAL "compatible")
  if(NOT _probe_found OR NOT _probe_version STREQUAL "0.2.0")
    message(
      FATAL_ERROR
        "${PS_VERSION_PROBE_PACKAGE} ${PS_VERSION_PROBE_VERSION} was not resolved to installed version 0.2.0"
    )
  endif()
else()
  if(_probe_found)
    message(
      FATAL_ERROR
        "${PS_VERSION_PROBE_PACKAGE} unexpectedly accepted ${PS_VERSION_PROBE_VERSION}"
    )
  endif()
  if(NOT _probe_versions STREQUAL "0.2.0")
    message(
      FATAL_ERROR
        "${PS_VERSION_PROBE_PACKAGE} considered unexpected versions: ${_probe_versions}"
    )
  endif()
endif()
