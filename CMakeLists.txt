# Copyright (c) 2016 - 2021 Advanced Micro Devices, Inc. All Rights Reserved.
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

cmake_minimum_required(VERSION 3.16.8)
project(hip)

# sample command for hip-rocclr runtime, you'll need to have rocclr built
# ROCM_PATH is the path where ROCM is installed
#  For shared lib of hip-rocclr runtime
#    For release version
#      cmake -DHIP_COMMON_DIR="$HIP_DIR" -DAMD_OPENCL_PATH=$OPENCL_DIR -DROCCLR_PATH=$ROCCLR_DIR -DCMAKE_PREFIX_PATH="<ROCM_PATH>/" -DCMAKE_INSTALL_PREFIX=</where/to/install/hip> ..
#    For debug version
#      cmake -DHIP_COMMON_DIR="$HIP_DIR" -DAMD_OPENCL_PATH=$OPENCL_DIR -DROCCLR_PATH=$ROCCLR_DIR -DCMAKE_PREFIX_PATH="<ROCM_PATH>/" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=</where/to/install/hip> ..
#  For static lib of hip-rocclr runtime
#    For release version
#      cmake -DHIP_COMMON_DIR="$HIP_DIR" -DAMD_OPENCL_PATH=$OPENCL_DIR -DROCCLR_PATH=$ROCCLR_DIR -DBUILD_SHARED_LIBS=OFF -DCMAKE_PREFIX_PATH="<ROCM_PATH>/" -DCMAKE_INSTALL_PREFIX=</where/to/install/hip> ..
#    For debug version
#      cmake -DHIP_COMMON_DIR="$HIP_DIR" -DAMD_OPENCL_PATH=$OPENCL_DIR -DROCCLR_PATH=$ROCCLR_DIR -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="<ROCM_PATH>/" -DCMAKE_INSTALL_PREFIX=</where/to/install/hip> ..
#  If you don't specify CMAKE_INSTALL_PREFIX, hip-rocclr runtime will be installed to "<ROCM_PATH>/hip".
#  By default, CMake will search for a folder named vdi or ROCclr relative to the current path. Specify -DROCCLR_PATH=$ROCCLR_DIR if rocclr source is in obscure location.
#  By default, CMake will search for a folder named opencl or ROCm-OpenCL-Runtime relative to the current path. Specify -DAMD_OPENCL_PATH=$OPENCL_DIR if opencl source is in obscure location.
list(APPEND CMAKE_MODULE_PATH ${HIP_COMMON_DIR}/cmake)

# required to add the right link to libhsa-runtime in install/lib path
# CMAKE_PREFIX_PATH is used as rpath to search for libs outside HIP
set(CMAKE_INSTALL_RPATH "${CMAKE_PREFIX_PATH}/lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

#############################
# Options
#############################
option(BUILD_HIPIFY_CLANG "Enable building the CUDA->HIP converter" OFF)
option(__HIP_ENABLE_PCH "Enable/Disable pre-compiled hip headers" ON)
option(__HIP_ENABLE_RTC "Enable/Disable pre-processed hiprtc shared lib" ON)
option(__HIP_USE_CMPXCHG_FOR_FP_ATOMICS "Enable/Disable FP Atomics version" OFF)
option(HIP_OFFICIAL_BUILD "Enable/Disable for mainline/staging builds" OFF)
set(HIPCC_BIN_DIR "" CACHE STRING "HIPCC and HIPCONFIG binary directories")

if(__HIP_ENABLE_PCH)
  set(_pchStatus 1)
else()
  set(_pchStatus 0)
endif()

if(__HIP_USE_CMPXCHG_FOR_FP_ATOMICS)
  add_definitions(-D__HIP_USE_CMPXCHG_FOR_FP_ATOMICS)
endif()

message(STATUS "HIPCC_BIN_DIR found at ${HIPCC_BIN_DIR}")
message(STATUS "HIP_COMMON_DIR found at ${HIP_COMMON_DIR}")
set(HIP_COMMON_INCLUDE_DIR ${HIP_COMMON_DIR}/include)
set(HIP_COMMON_BIN_DIR ${HIP_COMMON_DIR}/bin)
set(__HIPCONFIG_EXECUTABLE__ ${HIP_COMMON_DIR}/bin/hipconfig)

#############################
# Setup config generation
#############################
string(TIMESTAMP _timestamp UTC)
set(_versionInfo "# Auto-generated by cmake\n")
set(_buildInfo "# Auto-generated by cmake on ${_timestamp} UTC\n")
macro(add_to_config _configfile _variable)
    set(${_configfile} "${${_configfile}}${_variable}=${${_variable}}\n")
endmacro()

#############################
# Setup version information
#############################
find_package(Perl REQUIRED)

# Determine HIP_BASE_VERSION
set(ENV{HIP_PATH} "")
execute_process(COMMAND ${PERL_EXECUTABLE} ${__HIPCONFIG_EXECUTABLE__} --version
  OUTPUT_VARIABLE HIP_BASE_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "." ";" VERSION_LIST ${HIP_BASE_VERSION})
list(GET VERSION_LIST 0 HIP_VERSION_MAJOR)
list(GET VERSION_LIST 1 HIP_VERSION_MINOR)
list(GET VERSION_LIST 2 HIP_VERSION_PATCH_GITHASH)
string(REPLACE "-" ";" VERSION_LIST ${HIP_VERSION_PATCH_GITHASH})
list(GET VERSION_LIST 0 HIP_VERSION_PATCH)
set(HIP_VERSION_GITDATE 0)

find_package(Git)

# FIXME: Two different version strings used.
# Below we use UNIX commands, not compatible with Windows.
if(GIT_FOUND)
  # use the commit date, instead of build date
  execute_process(COMMAND ${GIT_EXECUTABLE} show -s --format=%ct
    RESULT_VARIABLE git_result
    OUTPUT_VARIABLE git_output
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(git_result EQUAL 0)
    set(HIP_VERSION_UNIXDATE ${git_output})
  endif()

  # get date information based on UTC
  # use the last two digits of year + week number + day in the week as HIP_VERSION_GITDATE
  execute_process(COMMAND ${PERL_EXECUTABLE} "-MPOSIX=strftime" "-le" "print strftime \'%y%W%w\',gmtime(${HIP_VERSION_UNIXDATE})"
    RESULT_VARIABLE git_result
    OUTPUT_VARIABLE git_output
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(git_result EQUAL 0)
    set(HIP_VERSION_GITDATE ${git_output})
  endif()

  # get commit short hash
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    RESULT_VARIABLE git_result
    OUTPUT_VARIABLE git_output
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(git_result EQUAL 0)
    set(HIP_VERSION_GITHASH ${git_output})
  endif()

  set(HIP_VERSION_BUILD_ID 0)
  set(HIP_VERSION_BUILD_NAME "")
  if(NOT DEFINED ENV{HIP_OFFICIAL_BUILD} AND NOT HIP_OFFICIAL_BUILD)
    set(HIP_VERSION_PATCH ${HIP_VERSION_GITDATE})
  endif()

  if(DEFINED ENV{ROCM_LIBPATCH_VERSION})
    set(HIP_PACKAGING_VERSION_PATCH ${HIP_VERSION_PATCH}.$ENV{ROCM_LIBPATCH_VERSION})
  else()
    set(HIP_PACKAGING_VERSION_PATCH ${HIP_VERSION_PATCH}-${HIP_VERSION_GITHASH})
  endif()
else()
  # FIXME: Some parts depend on this being set.
  set(HIP_PACKAGING_VERSION_PATCH "0")
endif()

## Debian package specific variables
if ( DEFINED ENV{CPACK_DEBIAN_PACKAGE_RELEASE} )
  set ( CPACK_DEBIAN_PACKAGE_RELEASE $ENV{CPACK_DEBIAN_PACKAGE_RELEASE} )
else()
  set ( CPACK_DEBIAN_PACKAGE_RELEASE "local" )
endif()
message (STATUS "Using CPACK_DEBIAN_PACKAGE_RELEASE ${CPACK_DEBIAN_PACKAGE_RELEASE}" )

## RPM package specific variables
if ( DEFINED ENV{CPACK_RPM_PACKAGE_RELEASE} )
  set ( CPACK_RPM_PACKAGE_RELEASE $ENV{CPACK_RPM_PACKAGE_RELEASE} )
else()
  set ( CPACK_RPM_PACKAGE_RELEASE "local" )
endif()

## 'dist' breaks manual builds on debian systems due to empty Provides
execute_process( COMMAND rpm --eval %{?dist}
                 RESULT_VARIABLE PROC_RESULT
                 OUTPUT_VARIABLE EVAL_RESULT
                 OUTPUT_STRIP_TRAILING_WHITESPACE )

if ( PROC_RESULT EQUAL "0" AND NOT EVAL_RESULT STREQUAL "" )
  string ( APPEND CPACK_RPM_PACKAGE_RELEASE "%{?dist}" )
endif()
message(STATUS "CPACK_RPM_PACKAGE_RELEASE: ${CPACK_RPM_PACKAGE_RELEASE}")

add_to_config(_versionInfo HIP_PACKAGING_VERSION_PATCH)
add_to_config(_versionInfo CPACK_DEBIAN_PACKAGE_RELEASE)
add_to_config(_versionInfo CPACK_RPM_PACKAGE_RELEASE)

add_to_config(_versionInfo HIP_VERSION_MAJOR)
add_to_config(_versionInfo HIP_VERSION_MINOR)
add_to_config(_versionInfo HIP_VERSION_PATCH)
add_to_config(_versionInfo HIP_VERSION_GITHASH)

set (HIP_LIB_VERSION_MAJOR ${HIP_VERSION_MAJOR})
set (HIP_LIB_VERSION_MINOR ${HIP_VERSION_MINOR})
if (${ROCM_PATCH_VERSION} )
   set (HIP_LIB_VERSION_PATCH ${ROCM_PATCH_VERSION})
else ()
   set (HIP_LIB_VERSION_PATCH ${HIP_VERSION_PATCH}-${HIP_VERSION_GITHASH})
endif ()
set (HIP_LIB_VERSION_STRING "${HIP_LIB_VERSION_MAJOR}.${HIP_LIB_VERSION_MINOR}.${HIP_LIB_VERSION_PATCH}")
if (DEFINED ENV{ROCM_RPATH})
    set (CMAKE_INSTALL_RPATH "$ENV{ROCM_RPATH}")
    set (CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set (CMAKE_SKIP_BUILD_RPATH TRUE)
endif ()

# overwrite HIP_VERSION_PATCH for packaging
set(HIP_VERSION ${HIP_VERSION_MAJOR}.${HIP_VERSION_MINOR}.${HIP_PACKAGING_VERSION_PATCH})

# Remove when CI is updated
if(HIP_PLATFORM STREQUAL "rocclr")
   set(HIP_PLATFORM "amd")
endif()
#############################
# Configure variables
#############################
# Determine HIP_PLATFORM
if(NOT DEFINED HIP_PLATFORM)
    if(NOT DEFINED ENV{HIP_PLATFORM})
        execute_process(COMMAND ${__HIPCONFIG_EXECUTABLE__} --platform
            OUTPUT_VARIABLE HIP_PLATFORM
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    else()
        set(HIP_PLATFORM $ENV{HIP_PLATFORM} CACHE STRING "HIP Platform")
    endif()
endif()
message(STATUS "HIP Platform: " ${HIP_PLATFORM})

if(HIP_PLATFORM STREQUAL "nvidia")
    set(HIP_RUNTIME "cuda"  CACHE STRING "HIP Runtime")
    set(HIP_COMPILER "nvcc" CACHE STRING "HIP Compiler")
elseif(HIP_PLATFORM STREQUAL "amd")
    set(HIP_RUNTIME "rocclr" CACHE STRING "HIP Runtime")
    set(HIP_COMPILER "clang" CACHE STRING "HIP Compiler")
else()
    message(FATAL_ERROR "Unexpected HIP_PLATFORM: " ${HIP_PLATFORM})
endif()

message(STATUS "HIP Runtime: " ${HIP_RUNTIME})
message(STATUS "HIP Compiler: " ${HIP_COMPILER})

add_to_config(_buildInfo HIP_RUNTIME)
add_to_config(_buildInfo HIP_COMPILER)

# Set default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

if (NOT DEFINED ROCM_PATH )
     set ( ROCM_PATH "/opt/rocm"  CACHE STRING "Default ROCM installation directory." )
endif ()
message (STATUS "ROCM Installation path(ROCM_PATH): ${ROCM_PATH}")

# Determine HIP install path
if (UNIX)
    set(HIP_DEFAULT_INSTALL_PREFIX "${ROCM_PATH}/hip")
endif()
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX ${HIP_DEFAULT_INSTALL_PREFIX} CACHE PATH "Installation path for HIP" FORCE)
endif()

if(DEV_LOG_ENABLE MATCHES "yes")
  add_definitions(-DDEV_LOG_ENABLE)
endif()

# Set default install path as "${ROCM_PATH}/hip", can override the path from cmake build.
set(CPACK_INSTALL_PREFIX ${HIP_DEFAULT_INSTALL_PREFIX} CACHE PATH "Package Installation path for HIP")

if(IS_ABSOLUTE ${CMAKE_INSTALL_PREFIX})
    message(STATUS "HIP will be installed in: " ${CMAKE_INSTALL_PREFIX})
else()
    message(FATAL_ERROR "Don't know where to install HIP. Please specify absolute path using -DCMAKE_INSTALL_PREFIX")
endif()

# set the installation path for the installer package
set(CPACK_SET_DESTDIR ON CACHE BOOL "Installer package will install hip to CMAKE_INSTALL_PREFIX instead of CPACK_PACKAGING_INSTALL_PREFIX")
if (NOT CPACK_SET_DESTDIR)
  set(CPACK_PACKAGING_INSTALL_PREFIX "${ROCM_PATH}/hip" CACHE PATH "Default installation path of hcc installer package")
endif (NOT CPACK_SET_DESTDIR)

#############################
# Build steps
#############################
set(BIN_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/bin)
set(LIB_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/lib)
set(INCLUDE_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/include)
set(CONFIG_PACKAGE_INSTALL_DIR ${LIB_INSTALL_DIR}/cmake/hip)
set(CONFIG_LANG_PACKAGE_INSTALL_DIR ${LIB_INSTALL_DIR}/cmake/hip-lang)

# Build clang hipify if enabled
if (BUILD_HIPIFY_CLANG)
    add_subdirectory(hipify-clang)
endif()

# Generate hip_version.h
set(_versionInfoHeader
"// Auto-generated by cmake\n
#ifndef HIP_VERSION_H
#define HIP_VERSION_H\n
#define HIP_VERSION_MAJOR ${HIP_VERSION_MAJOR}
#define HIP_VERSION_MINOR ${HIP_VERSION_MINOR}
#define HIP_VERSION_PATCH ${HIP_VERSION_PATCH}
#define HIP_VERSION_GITHASH \"${HIP_VERSION_GITHASH}\"
#define HIP_VERSION_BUILD_ID ${HIP_VERSION_BUILD_ID}
#define HIP_VERSION_BUILD_NAME \"${HIP_VERSION_BUILD_NAME}\"
#define HIP_VERSION    (HIP_VERSION_MAJOR * 10000000 + HIP_VERSION_MINOR * 100000 + HIP_VERSION_PATCH)\n
#define __HIP_HAS_GET_PCH ${_pchStatus}\n
#endif\n
")
file(WRITE "${PROJECT_BINARY_DIR}/include/hip/hip_version.h" ${_versionInfoHeader})

if(HIP_RUNTIME STREQUAL "rocclr")
	add_subdirectory(src)
endif()

# Generate .hipInfo
file(WRITE "${PROJECT_BINARY_DIR}/.hipInfo" ${_buildInfo})

# Generate .hipVersion
file(WRITE "${PROJECT_BINARY_DIR}/.hipVersion" ${_versionInfo})

# Build doxygen documentation
find_program(DOXYGEN_EXE doxygen)
if(DOXYGEN_EXE)
    add_custom_target(doc COMMAND HIP_PATH=${CMAKE_CURRENT_SOURCE_DIR} ${DOXYGEN_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/docs/doxygen-input/doxy.cfg
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/docs)
endif()

#############################
# Install steps
#############################

# Install .hipInfo
install(FILES ${PROJECT_BINARY_DIR}/.hipInfo DESTINATION lib)

# Install .hipVersion
install(FILES ${PROJECT_BINARY_DIR}/.hipVersion DESTINATION bin)

# Install src, bin, include & cmake if necessary
execute_process(COMMAND test ${CMAKE_INSTALL_PREFIX} -ef ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE INSTALL_SOURCE)
if(NOT ${INSTALL_SOURCE} EQUAL 0)
    if(WIN32)
      install(DIRECTORY ${HIP_COMMON_BIN_DIR} DESTINATION . USE_SOURCE_PERMISSIONS)
      if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/src/" DESTINATION bin
                FILES_MATCHING PATTERN "*.pdb"
                               PATTERN "*.ilk"
                               PATTERN "CMakeFiles" EXCLUDE
                               PATTERN "hip_rtc_gen" EXCLUDE
                               PATTERN "libelf" EXCLUDE
                               PATTERN "loader" EXCLUDE
                               PATTERN "pal" EXCLUDE
                               PATTERN "libamdhsacode" EXCLUDE)
      endif()
    else()
      # Exclude .bat files on Linux.
      install(DIRECTORY ${HIP_COMMON_BIN_DIR} DESTINATION . USE_SOURCE_PERMISSIONS
              PATTERN *.bat EXCLUDE)
    endif()

    # The following two lines will be removed after upstream updation
    install(CODE "MESSAGE(\"Removing ${CMAKE_INSTALL_PREFIX}/include\")")
    install(CODE "file(REMOVE_RECURSE ${CMAKE_INSTALL_PREFIX}/include)")

    install(DIRECTORY include DESTINATION .)
    install(DIRECTORY ${HIP_COMMON_INCLUDE_DIR}/hip/ DESTINATION include/hip/)
    install(DIRECTORY ${HIP_COMMON_DIR}/cmake DESTINATION .)
endif()

# Install generated headers
# FIXME: Associate with individual targets.
if(HIP_PLATFORM STREQUAL "amd")
install(FILES ${PROJECT_BINARY_DIR}/include/hip/amd_detail/hip_prof_str.h
        DESTINATION include/hip/amd_detail)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin DESTINATION . USE_SOURCE_PERMISSIONS)
endif()
install(FILES ${PROJECT_BINARY_DIR}/include/hip/hip_version.h
        DESTINATION include/hip)

#############################
# hip-config
#############################
include(CMakePackageConfigHelpers)

configure_package_config_file(
    hip-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/hip-config.cmake
    INSTALL_DESTINATION ${CONFIG_PACKAGE_INSTALL_DIR}
    PATH_VARS LIB_INSTALL_DIR INCLUDE_INSTALL_DIR BIN_INSTALL_DIR
    )

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/hip-config-version.cmake
    VERSION "${HIP_VERSION_MAJOR}.${HIP_VERSION_MINOR}.${HIP_VERSION_GITDATE}"
    COMPATIBILITY SameMajorVersion
    )
install(
    FILES
    ${CMAKE_CURRENT_BINARY_DIR}/hip-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/hip-config-version.cmake
    DESTINATION
    ${CONFIG_PACKAGE_INSTALL_DIR}
    )
# Packaging invokes UNIX commands, which are not available on Windows.

if(NOT WIN32)
    add_subdirectory(packaging)
endif()

#############################
# Code formatting
#############################
# Target: clangformat
find_program(CLANGFORMAT_EXE clang-format PATHS ${HCC_HOME}/bin)
if(CLANGFORMAT_EXE)
    file(GLOB_RECURSE FORMAT_SOURCE_FILE_LIST *.cpp *.hpp *.h)
    add_custom_target(clangformat COMMAND ${CLANGFORMAT_EXE} -style=file -i ${FORMAT_SOURCE_FILE_LIST}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

#############################
# Testing steps
#############################
# HIT is not compatible with Windows
if(NOT WIN32)
set(HIP_ROOT_DIR ${CMAKE_CURRENT_BINARY_DIR})
set(HIP_SRC_PATH ${CMAKE_CURRENT_SOURCE_DIR})
if(HIP_PLATFORM STREQUAL "nvidia")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory "${HIP_SRC_PATH}/include" "${HIP_ROOT_DIR}/include" RESULT_VARIABLE RUN_HIT ERROR_QUIET)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory "${HIP_COMMON_INCLUDE_DIR}/hip/" "${HIP_ROOT_DIR}/include/hip/" RESULT_VARIABLE RUN_HIT ERROR_QUIET)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory "${HIP_COMMON_DIR}/cmake" "${HIP_ROOT_DIR}/cmake" RESULT_VARIABLE RUN_HIT ERROR_QUIET)
if(${RUN_HIT} EQUAL 0)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_directory "${HIP_COMMON_BIN_DIR}" "${HIP_ROOT_DIR}/bin" RESULT_VARIABLE RUN_HIT ERROR_QUIET)
endif()
if(HIP_CATCH_TEST EQUAL "1")
    enable_testing()
    set(HIP_PATH ${HIP_ROOT_DIR})
    add_subdirectory(${HIP_COMMON_DIR}/tests/catch ${PROJECT_BINARY_DIR}/catch)
else()
    if(${RUN_HIT} EQUAL 0)
        set(CMAKE_MODULE_PATH "${HIP_ROOT_DIR}/cmake" ${CMAKE_MODULE_PATH})
        include(${HIP_COMMON_DIR}/tests/hit/HIT.cmake)
        include(${HIP_COMMON_DIR}/tests/Tests.cmake)
    else()
        message(STATUS "Testing targets will not be available. To enable them please ensure that the HIP installation directory is writeable. Use -DCMAKE_INSTALL_PREFIX to specify a suitable location")
    endif()
endif()
endif()

#############################
# Code analysis
#############################
# Target: clang
if(HIP_HIPCC_EXECUTABLE)
    add_custom_target(analyze
        COMMAND ${HIP_HIPCC_EXECUTABLE} -fvisibility=hidden -fvisibility-inlines-hidden --analyze --analyzer-outputtext  -isystem ${ROCM_PATH}/include -Wno-unused-command-line-argument -I${ROCM_PATH}/include -c  src/*.cpp -Iinclude/ -I./
    WORKING_DIRECTORY ${HIP_SRC_PATH})
    if(CPPCHECK_EXE)
        add_dependencies(analyze cppcheck)
    endif()
endif()

# vim: ts=4:sw=4:expandtab:smartindent
