# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
# otatool.py --port COM5 switch_ota_partition --slot 0
cmake_minimum_required(VERSION 3.5)

set(PROJECT_VER "2.0.0.1")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

#set(EXTRA_COMPONENT_DIRS ../esp-protocols/components)

set(EXTRA_COMPONENT_DIRS ../esp-protocols/components ../ESP32Components ../esp-idf-lib/components)

project(RelayController)