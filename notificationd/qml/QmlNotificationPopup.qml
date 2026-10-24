/*
 * Copyright (C) 2023-2024 LingmoOS Team.
 *
 * Author:     Reion Wong <reion@lingmoos.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

import QtQuick 2.12
import QtQuick.Controls 2.12
import QtQuick.Layouts 1.12
import QtQuick.Window 2.12
import Qt5Compat.GraphicalEffects
import LingmoUI.CompatibleModule 3.0 as LingmoUI
import Lingmo.Notification 1.0

NotificationPopup {
    id: control
    flags: Qt.WindowDoesNotAcceptFocus | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    width: 380
    height: _layout.implicitHeight + LingmoUI.Units.largeSpacing * 2
    color: "transparent"
    visible: false
    onVisibleChanged: if (visible) timer.restart()

    property int defaultTimeout: 7000
    // Longer when there is something to act on
    readonly property bool actionable: _actions.visible

    LingmoUI.WindowShadow {
        view: control
        radius: _background.radius
    }

    // "Vidro Lingmo" (Settings > Appearance > Glass): the bubble fills its
    // window and asks KWin for the blur; solid with Glass off
    LingmoUI.GlassSurface {
        id: _background
        anchors.fill: parent
        radius: LingmoUI.Theme.hugeRadius
        blurWindow: control
    }

    // Hovering anywhere (buttons included) holds the popup
    HoverHandler {
        id: _hover
        onHoveredChanged: {
            if (hovered)
                timer.stop()
            else if (!_actions.replying)
                timer.restart()
        }
    }

    MouseArea {
        id: _mouseArea
        anchors.fill: parent

        onClicked: {
            notificationsModel.activate(model.notificationId)
            notificationsModel.close(model.notificationId)
        }
    }

    ColumnLayout {
        id: _layout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: LingmoUI.Units.largeSpacing
        spacing: LingmoUI.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: LingmoUI.Units.largeSpacing

            Image {
                id: _icon
                Layout.preferredWidth: LingmoUI.Units.iconSizes.large * 0.8
                Layout.preferredHeight: Layout.preferredWidth
                Layout.alignment: Qt.AlignTop
                source: model.iconName ? "image://icontheme/%1".arg(model.iconName)
                                       : "image://icontheme/preferences-desktop-notification"
                sourceSize: Qt.size(Layout.preferredWidth, Layout.preferredWidth)
                antialiasing: true
                smooth: true
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                // App name · when
                RowLayout {
                    Layout.fillWidth: true
                    visible: model.appName !== ""

                    Label {
                        Layout.fillWidth: true
                        text: model.appName
                        elide: Text.ElideRight
                        color: LingmoUI.Theme.disabledTextColor
                        font.pointSize: LingmoUI.Theme.fontSize * 0.9
                    }

                    Label {
                        text: qsTr("now")
                        color: LingmoUI.Theme.disabledTextColor
                        font.pointSize: LingmoUI.Theme.fontSize * 0.9
                        // Room for the close button
                        rightPadding: 22
                    }
                }

                Label {
                    text: model.summary
                    visible: text
                    elide: Text.ElideRight
                    font.bold: true
                    Layout.fillWidth: true
                    rightPadding: LingmoUI.Units.largeSpacing
                }

                Label {
                    text: model.body.replace(/\n/g, "<br>")
                    // The server announces body-markup: <b>, <i>, <a>... (AutoText only looks at the first line)
                    textFormat: Text.StyledText
                    onLinkActivated: (link) => Qt.openUrlExternally(link)
                    visible: text
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
        }

        NotificationActions {
            id: _actions
            Layout.fillWidth: true
            Layout.leftMargin: LingmoUI.Units.iconSizes.large * 0.8 + LingmoUI.Units.largeSpacing
            Layout.topMargin: LingmoUI.Units.smallSpacing / 2
            z: 2
            actions: model.actions
            hasReply: model.hasReply
            replyPlaceholder: model.replyPlaceholder
            replySubmitText: model.replySubmitText

            onActionInvoked: (key) => notificationsModel.invokeAction(model.notificationId, key)
            onReplied: (text) => notificationsModel.reply(model.notificationId, text)
            onReplyStarted: {
                timer.stop()
                // The popup normally never takes focus; it has to for typing
                control.takeFocus()
            }
        }
    }

    // Close button, on hover
    Item {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: LingmoUI.Units.smallSpacing
        anchors.rightMargin: LingmoUI.Units.smallSpacing
        width: 22
        height: 22
        visible: _hover.hovered
        z: 9999

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: _closeBtnArea.containsMouse ? Qt.rgba(LingmoUI.Theme.textColor.r, LingmoUI.Theme.textColor.g,
                                                         LingmoUI.Theme.textColor.b, 0.12)
                                               : "transparent"
            Behavior on color { ColorAnimation { duration: 120 } }
        }

        Image {
            anchors.fill: parent
            source: "qrc:/images/" + (LingmoUI.Theme.darkMode ? "dark" : "light") + "/close.svg"
            sourceSize: Qt.size(width, height)
        }

        MouseArea {
            id: _closeBtnArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: {
                notificationsModel.close(model.notificationId)
            }
        }
    }

    Timer {
        id: timer
        interval: control.actionable ? control.defaultTimeout * 1.5 : control.defaultTimeout

        onTriggered: {
            notificationsModel.expired(model.notificationId)
        }
    }
}
