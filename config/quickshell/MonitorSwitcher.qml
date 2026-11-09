import Quickshell
import Quickshell.Io
import QtQuick
import QtQuick.Layouts
import QtQuick.Effects

// MonitorSwitcher — floating card for switching Hyprland monitor profiles.
// Toggle with:  quickshell -c default ipc call monitorSwitcher toggle
// Closes automatically ~500ms after a profile is applied.
// Palette: "Editor Dark" (bg #090b0d / fg #adbac7 / accents white+blue+muted-red)

Scope {
    id: root

    property bool open: false
    property bool busy: false
    property string activeProfile: "home"

    readonly property string scriptPath:
        Quickshell.env("HOME") + "/.config/quickshell/scripts/monitor-profile.sh"

    // palette
    readonly property color colBg: "#090b0d"
    readonly property color colPanel: "#121417"
    readonly property color colPanelAlt: "#181a1e"
    readonly property color colBorder: "#1e2025"
    readonly property color colBorderStrong: "#2d3035"
    readonly property color colFg: "#adbac7"
    readonly property color colFgDim: "#8a949e"
    readonly property color colWhite: "#e9eff6"
    readonly property color colBlue: "#7fa8c9"
    readonly property color colRed: "#b86a6a"
    readonly property color colInfo: "#64778d"

    readonly property var profiles: [
        { key: "home", label: "HOME",  sub: "1920×1080 · 110Hz",  tag: "H", accent: colBlue },
        { key: "work", label: "WORK",  sub: "1920×1080 · 75Hz",   tag: "W", accent: colWhite },
        { key: "auto", label: "AUTO",  sub: "preferred mode",     tag: "P", accent: colInfo }
    ]

    IpcHandler {
        target: "monitorSwitcher"
        function toggle(): void { root.open ? root.close() : root.reveal() }
        function show(): void { root.reveal() }
        function hide(): void { root.close() }
    }

    function reveal() {
        refreshProc.running = true
        root.open = true
    }

    function close() {
        root.open = false
    }

    Process {
        id: refreshProc
        command: ["bash", root.scriptPath, "--get"]
        stdout: SplitParser {
            onRead: data => {
                const p = data.trim()
                if (p.length > 0) root.activeProfile = p
            }
        }
    }

    Process {
        id: applyProc
        property string pendingKey: ""
        stdout: SplitParser { onRead: data => {} }
        onExited: (code) => {
            root.busy = false
            if (code === 0) {
                root.activeProfile = applyProc.pendingKey
                closeTimer.restart()
            } else {
                refreshProc.running = true
            }
        }
    }

    Timer {
        id: closeTimer
        interval: 500
        onTriggered: root.close()
    }

    function applyProfile(key) {
        if (root.busy || key === root.activeProfile) { root.close(); return }
        root.busy = true
        applyProc.pendingKey = key
        applyProc.command = ["bash", root.scriptPath, key]
        applyProc.running = true
    }

    Component.onCompleted: refreshProc.running = true

    PanelWindow {
        id: win
        visible: root.open || fade.animating || pop.animating
        color: "transparent"
        implicitWidth: 320
        implicitHeight: cardCol.implicitHeight + 36

        anchors { top: true; right: true }
        margins { top: 52; right: 20 }

        Rectangle {
            id: card
            anchors.fill: parent
            radius: 6
            color: root.colBg
            opacity: root.open ? 1 : 0
            scale: root.open ? 1 : 0.96
            transformOrigin: Item.Top

            Behavior on opacity {
                id: fade
                property bool animating: false
                NumberAnimation {
                    duration: root.open ? 140 : 160
                    easing.type: Easing.OutCubic
                    onRunningChanged: fade.animating = running
                }
            }
            Behavior on scale {
                id: pop
                property bool animating: false
                NumberAnimation {
                    duration: root.open ? 140 : 160
                    easing.type: Easing.OutCubic
                    onRunningChanged: pop.animating = running
                }
            }

            border.width: 1
            border.color: root.colBorder

            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: "#00000090"
                shadowBlur: 0.6
                shadowVerticalOffset: 6
            }

            ColumnLayout {
                id: cardCol
                anchors.fill: parent
                anchors.margins: 16
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Text {
                        text: "DISPLAY"
                        color: root.colFgDim
                        font.family: "monospace"
                        font.pixelSize: 11
                        font.letterSpacing: 2
                        Layout.fillWidth: true
                    }

                    Rectangle {
                        width: 6; height: 6; radius: 3
                        color: root.busy ? root.colWhite : root.colBlue
                        Behavior on color { ColorAnimation { duration: 180 } }
                        SequentialAnimation on opacity {
                            running: root.busy
                            loops: Animation.Infinite
                            NumberAnimation { to: 0.25; duration: 450 }
                            NumberAnimation { to: 1.0; duration: 450 }
                        }
                    }

                    Rectangle {
                        id: closeBtn
                        width: 20; height: 20; radius: 3
                        color: closeHover.hovered ? root.colPanelAlt : "transparent"
                        border.width: 1
                        border.color: closeHover.hovered ? root.colRed : root.colBorderStrong

                        Behavior on color { ColorAnimation { duration: 100 } }
                        Behavior on border.color { ColorAnimation { duration: 100 } }

                        Text {
                            anchors.centerIn: parent
                            text: "×"
                            color: closeHover.hovered ? root.colRed : root.colFgDim
                            font.family: "monospace"
                            font.pixelSize: 13
                            font.bold: true
                        }

                        HoverHandler { id: closeHover }
                        TapHandler { onTapped: root.close() }

                        scale: closePress.pressed ? 0.9 : 1.0
                        Behavior on scale { NumberAnimation { duration: 80 } }
                        TapHandler { id: closePress }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: root.colBorder
                }

                Repeater {
                    model: root.profiles

                    delegate: Rectangle {
                        id: item
                        required property var modelData
                        readonly property bool active: root.activeProfile === modelData.key

                        Layout.fillWidth: true
                        Layout.preferredHeight: 46
                        radius: 4
                        color: active ? root.colPanelAlt : (hover.hovered ? root.colPanel : "transparent")
                        border.width: 1
                        border.color: active ? item.modelData.accent : "transparent"

                        Behavior on color { ColorAnimation { duration: 120 } }
                        Behavior on border.color { ColorAnimation { duration: 120 } }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 12

                            Rectangle {
                                width: 22; height: 22; radius: 3
                                color: "transparent"
                                border.width: 1
                                border.color: item.active ? item.modelData.accent : root.colBorderStrong

                                Text {
                                    anchors.centerIn: parent
                                    text: item.modelData.tag
                                    color: item.active ? item.modelData.accent : root.colFgDim
                                    font.family: "monospace"
                                    font.pixelSize: 11
                                    font.bold: true
                                }
                            }

                            ColumnLayout {
                                spacing: 1
                                Layout.fillWidth: true

                                Text {
                                    text: item.modelData.label
                                    color: root.colFg
                                    font.family: "monospace"
                                    font.pixelSize: 12
                                    font.letterSpacing: 1
                                }
                                Text {
                                    text: item.modelData.sub
                                    color: root.colFgDim
                                    font.family: "monospace"
                                    font.pixelSize: 10
                                }
                            }

                            Text {
                                visible: item.active
                                text: "●"
                                color: item.modelData.accent
                                font.pixelSize: 10
                            }
                        }

                        HoverHandler { id: hover }
                        TapHandler {
                            enabled: !root.busy
                            onTapped: root.applyProfile(item.modelData.key)
                        }

                        scale: presser.pressed ? 0.98 : 1.0
                        Behavior on scale { NumberAnimation { duration: 80 } }
                        TapHandler { id: presser }
                    }
                }

                Text {
                    Layout.topMargin: 2
                    Layout.fillWidth: true
                    text: "instant · auto-close"
                    color: root.colFgDim
                    font.family: "monospace"
                    font.pixelSize: 9
                    opacity: 0.7
                    horizontalAlignment: Text.AlignHCenter
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }
    }
}
