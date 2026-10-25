QT -= core gui
CONFIG += console c++17
CONFIG -= app_bundle
TARGET = gamepad_motion
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
PROBE_SOURCE = $$OUT_PWD/motion_probe.cpp
!system(python $$system_quote($$PWD/generate.py) $$system_quote($$PROBE_SOURCE)): error(Cannot generate motion adapter test)
SOURCES += $$PROBE_SOURCE
