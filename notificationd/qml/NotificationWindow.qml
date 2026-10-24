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
import QtQml 2.12
import QtQuick.Controls 2.12
import QtQuick.Layouts 1.12
import QtQuick.Window 2.12
import Qt5Compat.GraphicalEffects
import LingmoUI.CompatibleModule 3.0 as LingmoUI
import Lingmo.Notification 1.0

Item {
    id: control

    visible: true

    readonly property color ink: LingmoUI.Theme.textColor

    // "Vidro Lingmo" (Settings > Appearance > Glass): the history panel fills
    // its window and asks KWin for the blur; solid with Glass off
    LingmoUI.GlassSurface {
        id: _background
        anchors.fill: parent
        radius: LingmoUI.Theme.hugeRadius * 1.3
        blurWindow: NotificationDialog
    }

    readonly property rect screenRect: {
        let rect = Qt.rect(screen.availableScreenRect.x,   // already in virtual desktop coordinates
                           screen.availableScreenRect.y,
                           screen.availableScreenRect.width,
                           screen.availableScreenRect.height)
        return rect
    }

    onScreenRectChanged: {
        NotificationDialog.width = 380
        NotificationDialog.height = screenRect.height - LingmoUI.Units.smallSpacing * 3
        NotificationDialog.x = screenRect.x + screenRect.width - NotificationDialog.width - LingmoUI.Units.smallSpacing * 1.5
        NotificationDialog.y = screenRect.y + LingmoUI.Units.smallSpacing * 1.5
    }

    ScreenHelper {
        id: screen
    }

    LingmoUI.WindowHelper {
        id: windowHelper
    }

    LingmoUI.WindowShadow {
        view: NotificationDialog
        radius: _background.radius
    }

    NumberAnimation {
        id: scrollToTopAni
        target: _view
        from: 0
        to: 0
        property: "contentY"
        duration: 200
        easing.type: Easing.OutSine
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: LingmoUI.Units.largeSpacing
        anchors.bottomMargin: LingmoUI.Units.largeSpacing
        spacing: LingmoUI.Units.largeSpacing

        RowLayout {
            Layout.leftMargin: LingmoUI.Units.largeSpacing
            Layout.rightMargin: LingmoUI.Units.largeSpacing

            Label {
                text: qsTr("Notification Center")
                Layout.fillWidth: true
                elide: Text.ElideRight
                leftPadding: LingmoUI.Units.smallSpacing
                color: LingmoUI.Theme.textColor
                font.bold: true
                font.pointSize: LingmoUI.Theme.fontSize * 1.4

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        if (_view.contentY === 0)
                            return

                        scrollToTopAni.from = _view.contentY
                        scrollToTopAni.to = 0
                        scrollToTopAni.restart()
                    }
                }
            }

            Button {
                visible: _view.count > 0
                text: qsTr("Clear all")
                onClicked: historyModel.clearAll()
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: _view
                anchors.fill: parent
                model: historyModel
                spacing: LingmoUI.Units.smallSpacing
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                leftMargin: LingmoUI.Units.largeSpacing
                rightMargin: LingmoUI.Units.largeSpacing

                ScrollBar.vertical: ScrollBar {}

                LingmoUI.WheelHandler { target: _view }

                Label {
                    anchors.centerIn: parent
                    text: qsTr("No notifications")
                    color: LingmoUI.Theme.disabledTextColor
                    font.pointSize: LingmoUI.Theme.fontSize * 1.3
                    visible: _view.count === 0
                }

                removeDisplaced: Transition {
                    NumberAnimation { properties: "x, y"; duration: 250 }
                }

                // One header per app: icon, name and a button that clears the group
                section.property: "group"
                section.criteria: ViewSection.FullString
                section.delegate: Item {
                    required property string section

                    width: ListView.view.width - ListView.view.leftMargin - ListView.view.rightMargin
                    height: 36

                    RowLayout {
                        anchors.fill: parent
                        anchors.topMargin: LingmoUI.Units.smallSpacing
                        anchors.leftMargin: LingmoUI.Units.smallSpacing
                        spacing: LingmoUI.Units.smallSpacing

                        Image {
                            readonly property string iconName: historyModel.groupIcon(parent.parent.section)
                            Layout.preferredWidth: LingmoUI.Units.iconSizes.small
                            Layout.preferredHeight: LingmoUI.Units.iconSizes.small
                            sourceSize: Qt.size(width, height)
                            source: "image://icontheme/" + (iconName ? iconName : "preferences-desktop-notification")
                        }

                        Label {
                            Layout.fillWidth: true
                            text: parent.parent.section !== "" ? parent.parent.section : qsTr("Other")
                            elide: Text.ElideRight
                            font.bold: true
                            color: LingmoUI.Theme.textColor
                        }

                        IconButton {
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 26
                            source: "qrc:/images/" + (LingmoUI.Theme.darkMode ? "dark" : "light") + "/clear.svg"
                            onLeftButtonClicked: historyModel.clearGroup(parent.parent.section)
                        }
                    }
                }

                delegate: Item {
                    id: _item

                    width: ListView.view.width - ListView.view.leftMargin - ListView.view.rightMargin
                    height: _itemLayout.implicitHeight + LingmoUI.Units.largeSpacing * 2

                    HoverHandler { id: _itemHover }

                    // A raised card on the glass
                    Rectangle {
                        anchors.fill: parent
                        color: LingmoUI.Theme.glassEnabled ? LingmoUI.Theme.glassRaisedColor
                                                           : LingmoUI.Theme.secondBackgroundColor
                        radius: LingmoUI.Theme.bigRadius
                        border.width: LingmoUI.Theme.glassEnabled ? 1 : 0
                        border.color: LingmoUI.Theme.glassBorderColor
                        opacity: _itemHover.hovered ? 1.0 : 0.8
                        Behavior on opacity { NumberAnimation { duration: LingmoUI.Units.veryShortDuration } }
                    }

                    MouseArea {
                        id: _itemMouseArea
                        anchors.fill: parent
                        onClicked: {
                            historyModel.activate(index)
                            NotificationDialog.hide()
                        }
                    }

                    ColumnLayout {
                        id: _itemLayout
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
                                Layout.preferredWidth: LingmoUI.Units.iconSizes.medium
                                Layout.preferredHeight: LingmoUI.Units.iconSizes.medium
                                Layout.alignment: Qt.AlignTop
                                source: model.iconName ? "image://icontheme/%1".arg(model.iconName)
                                                       : "image://icontheme/preferences-desktop-notification"
                                sourceSize: Qt.size(Layout.preferredWidth, Layout.preferredHeight)
                                antialiasing: true
                                smooth: true
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                RowLayout {
                                    Layout.fillWidth: true

                                    Label {
                                        text: model.summary
                                        visible: text
                                        elide: Text.ElideRight
                                        font.bold: true
                                        Layout.fillWidth: true
                                    }

                                    Label {
                                        text: model.created
                                        color: LingmoUI.Theme.disabledTextColor
                                        font.pointSize: LingmoUI.Theme.fontSize * 0.9
                                        rightPadding: _itemHover.hovered ? 22 : 0
                                    }
                                }

                                Label {
                                    id: bodyLabel
                                    text: model.body.replace(/\n/g, "<br>")
                                    // The server announces body-markup: <b>, <i>, <a>... (AutoText only looks at the first line)
                                    textFormat: Text.StyledText
                                    onLinkActivated: (link) => Qt.openUrlExternally(link)
                                    visible: text
                                    maximumLineCount: 3
                                    elide: Text.ElideRight
                                    wrapMode: Text.Wrap
                                    color: LingmoUI.Theme.textColor
                                    Layout.fillWidth: true
                                }
                            }
                        }

                        NotificationActions {
                            Layout.fillWidth: true
                            Layout.leftMargin: LingmoUI.Units.iconSizes.medium + LingmoUI.Units.largeSpacing
                            actions: model.actions
                            hasReply: model.hasReply
                            replyPlaceholder: model.replyPlaceholder
                            replySubmitText: model.replySubmitText
                            onActionInvoked: (key) => historyModel.invokeAction(index, key)
                            onReplied: (text) => historyModel.reply(index, text)
                        }
                    }

                    // Remove, on hover
                    Item {
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.topMargin: LingmoUI.Units.smallSpacing
                        anchors.rightMargin: LingmoUI.Units.smallSpacing
                        width: 22
                        height: 22
                        visible: _itemHover.hovered
                        z: 9999

                        Rectangle {
                            anchors.fill: parent
                            radius: height / 2
                            color: _closeBtnArea.containsMouse ? Qt.rgba(control.ink.r, control.ink.g, control.ink.b, 0.12)
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
                            onClicked: historyModel.remove(index)
                        }
                    }
                }
            }
        }
    }
}
