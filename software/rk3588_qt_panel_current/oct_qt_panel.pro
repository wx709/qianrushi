QT += widgets core

CONFIG += c++17
CONFIG -= app_bundle

TEMPLATE = app
TARGET = oct_qt_panel
DESTDIR = build
OBJECTS_DIR = build/obj
MOC_DIR = build/moc
RCC_DIR = build/rcc
UI_DIR = build/ui

SDK_INCLUDE_DIR = /usr/include/camcmosoctusb3
SDK_LIB_DIR = /usr/local/lib/camcmosoctusb3_1.2

INCLUDEPATH += $$SDK_INCLUDE_DIR
LIBS += -L$$SDK_LIB_DIR
LIBS += -lpthread -lrt -lgentlcamcmosoctusb3 -lcamcmosoctusb3

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    capture_worker.cpp

HEADERS += \
    mainwindow.h \
    capture_worker.h
