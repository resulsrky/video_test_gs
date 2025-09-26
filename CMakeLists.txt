cmake_minimum_required(VERSION 3.16)
project(video_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(CheckCXXCompilerFlag)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED
  gstreamer-1.0
  gstreamer-video-1.0
  gstreamer-rtp-1.0
  gstreamer-app-1.0
  gobject-2.0
  glib-2.0
)

add_executable(video_engine
  src/main.cpp
  src/logger.cpp
  src/utils.cpp
  src/qos_controller.cpp
  src/xor_fec.cpp
)

target_include_directories(video_engine PRIVATE
  include
  ${GSTREAMER_INCLUDE_DIRS}
)

target_link_directories(video_engine PRIVATE ${GSTREAMER_LIBRARY_DIRS})
target_compile_options(video_engine PRIVATE ${GSTREAMER_CFLAGS_OTHER})
target_link_libraries(video_engine PRIVATE ${GSTREAMER_LIBRARIES})

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions(video_engine PRIVATE VE_DEBUG)
endif()
