TEMPLATE = app
TARGET = moonlight-usb-host
CONFIG += console c++17
CONFIG -= qt app_bundle
include(../../globaldefs.pri)
SOURCES += main.cpp nativeusb.cpp
HEADERS += nativeusb.h
QMAKE_CXXFLAGS += -Wall -Wextra
# No Qt or AppImage loader environment is needed after pkexec sanitizes it.
usb-host-static:QMAKE_LFLAGS += -static-libstdc++ -static-libgcc
isEmpty(PREFIX): PREFIX = /usr/local
isEmpty(BINDIR): BINDIR = bin
target.path = $$PREFIX/$$BINDIR/
INSTALLS += target
