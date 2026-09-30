QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle

TARGET = file_mapping_transport
TEMPLATE = app

include(../../file-mapping/smoke/filemapping-transport.pri)
SOURCES += main.cpp
RESOURCES += ../clipboard_payload_routing/fixtures.qrc

# The probe links OpenSSL directly. Bundle its DLLs so the test does not rely
# on a developer's PATH or an unrelated OpenSSL installation on CI.
win32 {
    contains(QT_ARCH, arm64): transportTestArch = arm64
    else: transportTestArch = x64
    CONFIG(debug, debug|release): transportTestConfig = debug
    else: transportTestConfig = release
    transportTlsDlls = $$files($$PWD/../../libs/windows/lib/$$transportTestArch/libcrypto*.dll)
    transportTlsDlls += $$files($$PWD/../../libs/windows/lib/$$transportTestArch/libssl*.dll)
    for(transportTlsDll, transportTlsDlls) {
        QMAKE_POST_LINK += $$escape_expand(\\n\\t) $$QMAKE_COPY $$shell_quote($$shell_path($$transportTlsDll)) $$shell_quote($$shell_path($$OUT_PWD/$$transportTestConfig/))
    }
}
