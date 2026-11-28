win32 {
    contains(QT_ARCH, x86_64): PYROWAVE_ARCH = x64
    contains(QT_ARCH, arm64): PYROWAVE_ARCH = arm64
    PYROWAVE_DEPS = $$PWD/../../libs/windows
    INCLUDEPATH += $$PYROWAVE_DEPS/include $$PYROWAVE_DEPS/include/$$PYROWAVE_ARCH/pyrowave
    LIBS += -L$$PYROWAVE_DEPS/lib/$$PYROWAVE_ARCH -lpyrowave-shared
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += pyrowave-shared
}
