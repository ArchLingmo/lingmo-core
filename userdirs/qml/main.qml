/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LingmoUI.CompatibleModule 3.0 as LingmoUI

LingmoUI.Window {
    id: rootWindow

    readonly property bool languageMode: backend.mode === "language"

    width: 600
    height: 620
    minimumWidth: width
    maximumWidth: width
    minimumHeight: height
    maximumHeight: height
    visible: true
    minimizeButtonVisible: false
    title: languageMode ? qsTr("Folder names") : qsTr("Duplicate folders")

    // Never stop halfway: the window can't be closed while files move
    onClosing: (close) => {
        if (backend.page === "progress")
            close.accepted = false
    }

    background.opacity: LingmoUI.Theme.darkMode ? 0.88 : 0.97
    header.height: LingmoUI.Units.windowHeaderHeight
    contentTopMargin: 0

    LingmoUI.WindowBlur {
        view: rootWindow
        geometry: Qt.rect(rootWindow.x, rootWindow.y, rootWindow.width, rootWindow.height)
        windowRadius: rootWindow.windowRadius
        enabled: true
    }

    // ------------------------------------------------------------ question

    ColumnLayout {
        id: promptPage
        anchors.fill: parent
        anchors.topMargin: rootWindow.header.height
        anchors.leftMargin: LingmoUI.Units.largeSpacing * 2
        anchors.rightMargin: LingmoUI.Units.largeSpacing * 2
        anchors.bottomMargin: LingmoUI.Units.largeSpacing * 2
        spacing: LingmoUI.Units.largeSpacing
        visible: backend.page === "prompt"

        readonly property bool empty: folderList.count === 0

        RowLayout {
            Layout.fillWidth: true
            spacing: LingmoUI.Units.largeSpacing

            LingmoUI.IconItem {
                Layout.alignment: Qt.AlignTop
                width: LingmoUI.Units.iconSizes.large
                height: width
                Layout.preferredWidth: width
                Layout.preferredHeight: height
                source: "folder"
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: LingmoUI.Units.smallSpacing

                Label {
                    Layout.fillWidth: true
                    text: rootWindow.languageMode ? qsTr("Rename the folders to %1?").arg(backend.languageName)
                                                  : qsTr("Merge duplicate folders?")
                    font.pointSize: LingmoUI.Units.fontHeading
                    font.bold: true
                    color: LingmoUI.Theme.textColor
                    wrapMode: Text.Wrap
                }

                Label {
                    Layout.fillWidth: true
                    text: rootWindow.languageMode
                          ? qsTr("The system language changed. Your personal folders can use the names of the new language. Nothing is deleted: when a folder with the new name already exists, the contents are merged into it.")
                          : qsTr("Some personal folders exist twice, in different languages. Their contents can be merged into one folder. Nothing is overwritten: items with the same name are kept with a “(2)” suffix.")
                    font.pointSize: LingmoUI.Units.fontBody
                    color: LingmoUI.Theme.secondaryTextColor
                    wrapMode: Text.Wrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !rootWindow.languageMode
            spacing: LingmoUI.Units.largeSpacing

            Label {
                text: qsTr("Merge into")
                color: LingmoUI.Theme.textColor
                font.pointSize: LingmoUI.Units.fontBody
            }

            LingmoUI.SegmentedControl {
                Layout.fillWidth: true
                fillWidth: true
                model: [qsTr("Current folders"), qsTr("%1 names").arg(backend.languageName)]
                currentIndex: backend.mergeIntoLocalized ? 1 : 0
                onActivated: (index) => backend.mergeIntoLocalized = (index === 1)
            }
        }

        LingmoUI.Card {
            id: listCard
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: 0

            ListView {
                id: folderList
                anchors.fill: parent
                anchors.margins: LingmoUI.Units.smallSpacing
                clip: true
                model: backend.items
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                LingmoUI.WheelHandler {
                    target: folderList
                }

                delegate: Rectangle {
                    id: row
                    width: ListView.view.width
                    height: rowLayout.implicitHeight + LingmoUI.Units.largeSpacing
                    radius: LingmoUI.Theme.smallRadius
                    color: rowMouse.containsMouse && modelData.checkable ? LingmoUI.Theme.hoverColor : "transparent"

                    Behavior on color {
                        ColorAnimation { duration: LingmoUI.Units.veryShortDuration }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: modelData.checkable
                        onClicked: backend.setChecked(index, !modelData.checked)
                    }

                    RowLayout {
                        id: rowLayout
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: LingmoUI.Units.smallSpacing
                        anchors.rightMargin: LingmoUI.Units.largeSpacing
                        spacing: LingmoUI.Units.smallSpacing

                        CheckBox {
                            checked: modelData.checked
                            enabled: modelData.checkable
                            onToggled: backend.setChecked(index, checked)
                        }

                        LingmoUI.IconItem {
                            width: LingmoUI.Units.iconSizes.smallMedium
                            height: width
                            Layout.preferredWidth: width
                            Layout.preferredHeight: height
                            source: modelData.warning ? "dialog-warning" : "folder"
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: LingmoUI.Units.smallSpacing

                                Label {
                                    text: modelData.fromName
                                    color: LingmoUI.Theme.textColor
                                    font.pointSize: LingmoUI.Units.fontBody
                                    elide: Text.ElideMiddle
                                    Layout.maximumWidth: folderList.width * 0.36
                                }

                                LingmoUI.IconItem {
                                    width: 12
                                    height: 12
                                    Layout.preferredWidth: 12
                                    Layout.preferredHeight: 12
                                    source: "go-next-symbolic"
                                    color: LingmoUI.Theme.secondaryTextColor
                                }

                                Label {
                                    text: modelData.toName
                                    color: LingmoUI.Theme.textColor
                                    font.pointSize: LingmoUI.Units.fontBody
                                    font.bold: true
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: modelData.detail
                                color: modelData.warning ? LingmoUI.Theme.warningColor : LingmoUI.Theme.secondaryTextColor
                                font.pointSize: LingmoUI.Units.fontCaption
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }

            LingmoUI.EmptyState {
                anchors.centerIn: parent
                width: parent.width - LingmoUI.Units.largeSpacing * 4
                visible: promptPage.empty
                iconName: "folder"
                title: rootWindow.languageMode ? qsTr("Nothing to rename") : qsTr("No duplicate folders")
                text: rootWindow.languageMode ? qsTr("The folders already use the %1 names.").arg(backend.languageName)
                                              : qsTr("Each personal folder exists only once.")
            }
        }

        CheckBox {
            id: dontAsk
            visible: rootWindow.languageMode && !promptPage.empty
            text: qsTr("Don't ask again")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: LingmoUI.Units.largeSpacing

            Item { Layout.fillWidth: true }

            LingmoUI.ActionButton {
                visible: !promptPage.empty
                text: rootWindow.languageMode ? qsTr("Keep current names") : qsTr("Not now")
                onClicked: backend.keep(dontAsk.checked)
            }

            LingmoUI.ActionButton {
                primary: true
                text: promptPage.empty ? qsTr("Close")
                                       : rootWindow.languageMode ? qsTr("Rename") : qsTr("Merge folders")
                enabled: promptPage.empty || backend.checkedCount > 0
                onClicked: {
                    if (promptPage.empty) {
                        backend.close()
                        return
                    }
                    // "Don't ask again" also counts when renaming: the
                    // locale gets written either way
                    backend.apply()
                }
            }
        }
    }

    // ------------------------------------------------------------ working

    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - LingmoUI.Units.largeSpacing * 6
        spacing: LingmoUI.Units.largeSpacing
        visible: backend.page === "progress"

        LingmoUI.Spinner {
            Layout.alignment: Qt.AlignHCenter
            running: parent.visible
            size: 32
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: rootWindow.languageMode ? qsTr("Renaming folders…") : qsTr("Merging folders…")
            font.pointSize: LingmoUI.Units.fontHeading
            font.bold: true
            color: LingmoUI.Theme.textColor
        }

        ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: 1
            value: backend.progress
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: backend.currentPath
            elide: Text.ElideMiddle
            color: LingmoUI.Theme.secondaryTextColor
            font.pointSize: LingmoUI.Units.fontCaption
        }
    }

    // ------------------------------------------------------------ result

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: rootWindow.header.height
        anchors.leftMargin: LingmoUI.Units.largeSpacing * 2
        anchors.rightMargin: LingmoUI.Units.largeSpacing * 2
        anchors.bottomMargin: LingmoUI.Units.largeSpacing * 2
        spacing: LingmoUI.Units.largeSpacing
        visible: backend.page === "summary" || backend.page === "undone"

        Item { Layout.fillHeight: true }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 64
            height: 64
            radius: 32
            color: backend.errors.length > 0 ? LingmoUI.Theme.warningColor : LingmoUI.Theme.successColor

            LingmoUI.IconItem {
                anchors.centerIn: parent
                width: 32
                height: 32
                source: backend.page === "undone" ? "edit-undo-symbolic"
                                                  : backend.errors.length > 0 ? "dialog-warning-symbolic" : "emblem-ok-symbolic"
                color: LingmoUI.Theme.highlightedTextColor
            }
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: backend.summaryTitle
            font.pointSize: LingmoUI.Units.fontHeading
            font.bold: true
            color: LingmoUI.Theme.textColor
            wrapMode: Text.Wrap
        }

        LingmoUI.Card {
            Layout.fillWidth: true
            visible: backend.summaryDetails.length > 0

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: LingmoUI.Units.largeSpacing
                anchors.leftMargin: parent.padding
                anchors.rightMargin: parent.padding
                spacing: LingmoUI.Units.smallSpacing

                Repeater {
                    model: backend.summaryDetails

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: LingmoUI.Units.smallSpacing

                        Rectangle {
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 7
                            width: 5
                            height: 5
                            radius: 2.5
                            color: LingmoUI.Theme.highlightColor
                        }

                        Label {
                            Layout.fillWidth: true
                            text: modelData
                            color: LingmoUI.Theme.textColor
                            font.pointSize: LingmoUI.Units.fontBody
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }

        LingmoUI.Banner {
            Layout.fillWidth: true
            type: LingmoUI.Banner.Warning
            title: backend.errors.length > 0 ? qsTr("Some items stayed where they were") : ""
            text: backend.errors.slice(0, 4).join("\n")
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            spacing: LingmoUI.Units.largeSpacing

            Item { Layout.fillWidth: true }

            LingmoUI.ActionButton {
                visible: backend.canUndo
                text: qsTr("Undo")
                iconName: "edit-undo"
                onClicked: backend.undoLast()
            }

            LingmoUI.ActionButton {
                primary: true
                text: qsTr("Done")
                onClicked: backend.close()
            }
        }
    }
}
