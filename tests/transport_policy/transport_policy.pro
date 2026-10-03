QT += core testlib
CONFIG += console c++17
CONFIG -= app_bundle
INCLUDEPATH += ../../app
SOURCES += main.cpp ../../app/backend/transportpolicy.cpp ../../app/backend/transportpolicymirror.cpp ../../app/backend/transportpolicycontroller.cpp
HEADERS += ../../app/backend/transportpolicy.h ../../app/backend/transportpolicymirror.h ../../app/backend/transportpolicycontroller.h
