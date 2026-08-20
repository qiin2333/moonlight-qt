import QtQuick
import QtQuick.Window

Item {
    id: resizeArea

    required property Window window
    property int edgeSize: 7
    property real topInset: 0

    anchors.fill: parent
    anchors.topMargin: -topInset
    visible: Qt.platform.os === "windows" && window.visibility === Window.Windowed

    component ResizeHandle: MouseArea {
        required property Window resizeWindow
        required property int edges

        acceptedButtons: Qt.LeftButton
        preventStealing: true
        onPressed: resizeWindow.startSystemResize(edges)
    }

    ResizeHandle {
        resizeWindow: resizeArea.window
        edges: Qt.TopEdge | Qt.LeftEdge
        width: resizeArea.edgeSize
        height: resizeArea.edgeSize
        anchors.left: parent.left
        anchors.top: parent.top
        cursorShape: Qt.SizeFDiagCursor
        z: 2
    }

    ResizeHandle {
        resizeWindow: resizeArea.window
        edges: Qt.TopEdge | Qt.RightEdge
        width: resizeArea.edgeSize
        height: resizeArea.edgeSize
        anchors.right: parent.right
        anchors.top: parent.top
        cursorShape: Qt.SizeBDiagCursor
        z: 2
    }

    ResizeHandle {
        resizeWindow: resizeArea.window
        edges: Qt.BottomEdge | Qt.LeftEdge
        width: resizeArea.edgeSize
        height: resizeArea.edgeSize
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        cursorShape: Qt.SizeBDiagCursor
        z: 2
    }

    ResizeHandle {
        resizeWindow: resizeArea.window
        edges: Qt.BottomEdge | Qt.RightEdge
        width: resizeArea.edgeSize
        height: resizeArea.edgeSize
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        cursorShape: Qt.SizeFDiagCursor
        z: 2
    }

    ResizeHandle {
        resizeWindow: resizeArea.window
        edges: Qt.TopEdge
        height: resizeArea.edgeSize
        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            leftMargin: resizeArea.edgeSize
            rightMargin: resizeArea.edgeSize
        }
        cursorShape: Qt.SizeVerCursor
    }

    ResizeHandle {
        resizeWindow: resizeArea.window
        edges: Qt.BottomEdge
        height: resizeArea.edgeSize
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            leftMargin: resizeArea.edgeSize
            rightMargin: resizeArea.edgeSize
        }
        cursorShape: Qt.SizeVerCursor
    }

    ResizeHandle {
        resizeWindow: resizeArea.window
        edges: Qt.LeftEdge
        width: resizeArea.edgeSize
        anchors {
            left: parent.left
            top: parent.top
            bottom: parent.bottom
            topMargin: resizeArea.edgeSize
            bottomMargin: resizeArea.edgeSize
        }
        cursorShape: Qt.SizeHorCursor
    }

    ResizeHandle {
        resizeWindow: resizeArea.window
        edges: Qt.RightEdge
        width: resizeArea.edgeSize
        anchors {
            right: parent.right
            top: parent.top
            bottom: parent.bottom
            topMargin: resizeArea.edgeSize
            bottomMargin: resizeArea.edgeSize
        }
        cursorShape: Qt.SizeHorCursor
    }
}
