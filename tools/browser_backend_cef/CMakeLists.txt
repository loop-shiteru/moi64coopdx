cmake_minimum_required(VERSION 3.20)

project(browser_backend_cef LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (MSVC)
  add_compile_options(/MP)
endif()

if (NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "browser_backend_cef only supports Windows and Linux builds.")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if (WIN32)
  set(DEFAULT_CEF_ROOT "${REPO_ROOT}/lib/cef/win64")
  set(CEF_LIBRARY_NAME "libcef.dll")
  set(CEF_IMPORT_LIBRARY_NAME "libcef.lib")
else()
  set(DEFAULT_CEF_ROOT "${REPO_ROOT}/lib/cef/linux64")
  set(CEF_LIBRARY_NAME "libcef.so")
endif()

set(CEF_ROOT "${DEFAULT_CEF_ROOT}" CACHE PATH "CEF SDK root")
set(CEF_OUTPUT_DIR "${CEF_ROOT}/bridge" CACHE PATH "Output directory for bridge artifacts")

if (NOT EXISTS "${CEF_ROOT}/include/cef_app.h")
  message(FATAL_ERROR "CEF headers were not found at ${CEF_ROOT}/include")
endif()

if (WIN32)
  if (NOT EXISTS "${CEF_ROOT}/Release/${CEF_IMPORT_LIBRARY_NAME}")
    message(FATAL_ERROR "${CEF_IMPORT_LIBRARY_NAME} was not found at ${CEF_ROOT}/Release")
  endif()
  if (NOT EXISTS "${CEF_ROOT}/Release/${CEF_LIBRARY_NAME}")
    message(FATAL_ERROR "${CEF_LIBRARY_NAME} was not found at ${CEF_ROOT}/Release")
  endif()
else()
  if (NOT EXISTS "${CEF_ROOT}/Release/${CEF_LIBRARY_NAME}")
    message(FATAL_ERROR "${CEF_LIBRARY_NAME} was not found at ${CEF_ROOT}/Release")
  endif()
endif()

file(MAKE_DIRECTORY "${CEF_OUTPUT_DIR}")

function(SET_LIBRARY_TARGET_PROPERTIES target)
  target_include_directories(${target} PUBLIC "${CEF_ROOT}" "${CEF_ROOT}/include")
  target_compile_features(${target} PUBLIC cxx_std_20)

  if (WIN32)
    target_compile_definitions(${target} PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  endif()
endfunction()

function(set_bridge_output_dirs target)
  set_target_properties(${target} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CEF_OUTPUT_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CEF_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CEF_OUTPUT_DIR}"
  )

  foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(${target} PROPERTIES
      ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CEF_OUTPUT_DIR}"
      LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${CEF_OUTPUT_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CEF_OUTPUT_DIR}"
    )
  endforeach()
endfunction()

function(set_cef_origin_rpath target)
  if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(${target} PROPERTIES
      BUILD_RPATH "$ORIGIN"
      INSTALL_RPATH "$ORIGIN"
    )
  endif()
endfunction()

add_subdirectory("${CEF_ROOT}/libcef_dll" "${CMAKE_CURRENT_BINARY_DIR}/libcef_dll")

add_library(cef_binary SHARED IMPORTED GLOBAL)
if (WIN32)
  set_target_properties(cef_binary PROPERTIES
    IMPORTED_IMPLIB "${CEF_ROOT}/Release/${CEF_IMPORT_LIBRARY_NAME}"
    IMPORTED_LOCATION "${CEF_ROOT}/Release/${CEF_LIBRARY_NAME}"
  )
else()
  set_target_properties(cef_binary PROPERTIES
    IMPORTED_LOCATION "${CEF_ROOT}/Release/${CEF_LIBRARY_NAME}"
  )
endif()

add_library(browser_backend_cef SHARED
  src/browser_backend_cef.cpp
  "${REPO_ROOT}/src/pc/utils/miniaudio.c"
)
target_include_directories(browser_backend_cef PRIVATE
  "${REPO_ROOT}"
  "${REPO_ROOT}/include"
  "${REPO_ROOT}/src"
  "${CEF_ROOT}"
  "${CEF_ROOT}/include"
)
target_compile_definitions(browser_backend_cef PRIVATE MA_NO_DECODING)

if (WIN32)
  target_compile_definitions(browser_backend_cef PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(browser_backend_cef PRIVATE cef_binary libcef_dll_wrapper ole32 uuid winmm avrt)
else()
  find_package(Threads REQUIRED)
  find_package(X11 REQUIRED)
  target_link_libraries(browser_backend_cef PRIVATE cef_binary libcef_dll_wrapper Threads::Threads X11::X11 dl)
  set_target_properties(browser_backend_cef PROPERTIES PREFIX "")
endif()

set_bridge_output_dirs(browser_backend_cef)
set_cef_origin_rpath(browser_backend_cef)

if (WIN32)
  add_library(browser_subprocess SHARED
    src/browser_subprocess.cpp
  )
  target_include_directories(browser_subprocess PRIVATE
    "${CEF_ROOT}"
    "${CEF_ROOT}/include"
  )
  target_compile_definitions(browser_subprocess PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  target_link_libraries(browser_subprocess PRIVATE cef_binary libcef_dll_wrapper)
else()
  add_executable(browser_subprocess
    src/browser_subprocess.cpp
  )
  target_include_directories(browser_subprocess PRIVATE
    "${CEF_ROOT}"
    "${CEF_ROOT}/include"
  )
  target_link_libraries(browser_subprocess PRIVATE cef_binary libcef_dll_wrapper Threads::Threads X11::X11 dl)
endif()

set_bridge_output_dirs(browser_subprocess)
set_cef_origin_rpath(browser_subprocess)
