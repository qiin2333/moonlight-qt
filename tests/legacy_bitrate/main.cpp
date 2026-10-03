#include "backend/legacybitrate.h"
#include "backend/legacytransportscope.h"
#include <QtTest>

class LegacyBitrateTest : public QObject
{
    Q_OBJECT
private slots:
    void scopeHandshakePreservesUnsignedIdentity()
    {
        const auto scope = parseLegacyTransportScope(
            QStringLiteral("<root "
                           "status_code=\"200\"><gamesession>1</gamesession><transportScope>1</"
                           "transportScope><transportSessionId>4294967295</"
                           "transportSessionId><transportConnectionEpoch>18446744073709551615</"
                           "transportConnectionEpoch></root>"));
        QVERIFY(scope);
        QCOMPARE(scope->querySuffix(),
                 QStringLiteral("&sessionId=4294967295&connectionEpoch=18446744073709551615"));
        QJsonObject body;
        body["enabled"] = false;
        scope->appendTo(body);
        QCOMPARE(body["connectionEpoch"].toString(), QStringLiteral("18446744073709551615"));
        QVERIFY(!parseLegacyTransportScope(QStringLiteral(
            "<root status_code=\"200\"><transportSessionId>1</transportSessionId></root>")));
    }
    void scopeHandshakeCannotSilentlyDowngrade_data()
    {
        QTest::addColumn<QString>("xml");
        const auto row = [](const char* name, const char* fields) {
            QTest::newRow(name) << QStringLiteral("<root>") + QString::fromUtf8(fields) +
                                       QStringLiteral("</root>");
        };
        row("missing ack", "<transportSessionId>1</transportSessionId><transportConnectionEpoch>2</"
                           "transportConnectionEpoch>");
        row("missing epoch",
            "<transportScope>1</transportScope><transportSessionId>1</transportSessionId>");
        row("missing session", "<transportScope>1</transportScope><transportConnectionEpoch>2</"
                               "transportConnectionEpoch>");
        row("unknown ack",
            "<transportScope>2</transportScope><transportSessionId>1</"
            "transportSessionId><transportConnectionEpoch>2</transportConnectionEpoch>");
        row("duplicate epoch",
            "<transportScope>1</transportScope><transportSessionId>1</"
            "transportSessionId><transportConnectionEpoch>2</"
            "transportConnectionEpoch><transportConnectionEpoch>3</transportConnectionEpoch>");
        row("zero epoch",
            "<transportScope>1</transportScope><transportSessionId>1</"
            "transportSessionId><transportConnectionEpoch>0</transportConnectionEpoch>");
        row("overflow session",
            "<transportScope>1</transportScope><transportSessionId>4294967296</"
            "transportSessionId><transportConnectionEpoch>2</transportConnectionEpoch>");
        row("leading zero",
            "<transportScope>1</transportScope><transportSessionId>1</"
            "transportSessionId><transportConnectionEpoch>02</transportConnectionEpoch>");
    }
    void scopeHandshakeCannotSilentlyDowngrade()
    {
        QFETCH(QString, xml);
        QVERIFY_EXCEPTION_THROWN(parseLegacyTransportScope(xml), std::invalid_argument);
    }
    void validatesSunshineReplyShape()
    {
        // Sunshine's legacy response shape, including the echoed requested target.
        const auto accepted =
            acceptedLegacyBitrate(QStringLiteral("<?xml version=\"1.0\"?><root status_code=\"200\" "
                                                 "bitrate=\"4500\" clientname=\"validation\" "
                                                 "status_message=\"Bitrate change request sent to "
                                                 "client session\"><bitrate>1</bitrate></root>"),
                                  4500);
        QVERIFY(accepted);
        QCOMPARE(*accepted, 4500);
    }
    void retainsOldHostWithoutEcho()
    {
        const auto accepted = acceptedLegacyBitrate(
            QStringLiteral("<root status_code=\"200\"><bitrate>1</bitrate></root>"), 6000);
        QVERIFY(accepted);
        QCOMPARE(*accepted, 6000);
    }
    void rejectsFailureMalformedOrMismatchedReplies_data()
    {
        QTest::addColumn<QString>("xml");
        const auto row = [](const char* name, const char* xml) {
            QTest::newRow(name) << QString::fromUtf8(xml);
        };
        row("no session", "<root status_code=\"404\"><bitrate>0</bitrate></root>");
        row("explicit failure", "<root status_code=\"200\"><bitrate>0</bitrate></root>");
        row("missing flag", "<root status_code=\"200\"/>");
        row("missing status", "<root><bitrate>1</bitrate></root>");
        row("wrong target",
            "<root status_code=\"200\" bitrate=\"4000\"><bitrate>1</bitrate></root>");
        row("nonnumeric target",
            "<root status_code=\"200\" bitrate=\"six\"><bitrate>1</bitrate></root>");
        row("duplicate flags",
            "<root status_code=\"200\"><bitrate>0</bitrate><bitrate>1</bitrate></root>");
        row("truncated", "<root status_code=\"200\"><bitrate>1</bitrate>");
        row("second root", "<root status_code=\"200\"><bitrate>1</bitrate></root><root/>");
    }
    void rejectsFailureMalformedOrMismatchedReplies()
    {
        QFETCH(QString, xml);
        QVERIFY(!acceptedLegacyBitrate(xml, 6000));
    }
    void rejectsInvalidRequestedTarget()
    {
        const auto xml = QStringLiteral("<root status_code=\"200\"><bitrate>1</bitrate></root>");
        QVERIFY(!acceptedLegacyBitrate(xml, 0));
        QVERIFY(!acceptedLegacyBitrate(xml, 800001));
    }
};
QTEST_GUILESS_MAIN(LegacyBitrateTest)
#include "main.moc"
