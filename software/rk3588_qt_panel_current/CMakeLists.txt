cmake_minimum_required(VERSION 3.16)
project(oct_qt_panel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC ON)

set(SDK_INCLUDE_DIR "/usr/include/camcmosoctusb3" CACHE PATH "Path to camcmosoctusb3 headers")
set(SDK_LIB_DIR "/usr/local/lib/camcmosoctusb3_1.2" CACHE PATH "Path to camcmosoctusb3 libs")

find_package(Qt6 COMPONENTS Widgets Core QUIET)
if(Qt6_FOUND)
  set(QT_LIBS Qt6::Widgets Qt6::Core)
else()
  find_package(Qt5 REQUIRED COMPONENTS Widgets Core)
  set(QT_LIBS Qt5::Widgets Qt5::Core)
endif()

add_executable(oct_qt_panel
  main.cpp
  mainwindow.cpp
  mainwindow.h
  capture_worker.cpp
  capture_worker.h
)

target_include_directories(oct_qt_panel PRIVATE "${SDK_INCLUDE_DIR}")
target_link_directories(oct_qt_panel PRIVATE "${SDK_LIB_DIR}")

target_link_libraries(oct_qt_panel
  PRIVATE
    ${QT_LIBS}
    pthread
    rt
    gentlcamcmosoctusb3
    camcmosoctusb3
)
