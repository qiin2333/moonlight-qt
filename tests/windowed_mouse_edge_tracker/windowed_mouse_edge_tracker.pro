TEMPLATE = app
TARGET = windowed_mouse_edge_tracker

QT -= gui
CONFIG += console c++17
CONFIG -= app_bundle

# Compile the production helper directly so this regression test exercises the
# same edge calculations that are used by the streaming input path.
SOURCES += \
    main.cpp \
    ../../app/streaming/input/windowedmouseedgetracker.cpp

HEADERS += \
    ../../app/streaming/input/windowedmouseedgetracker.h

INCLUDEPATH += ../../app/streaming/input
