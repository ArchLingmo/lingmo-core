/*
 * Copyright (C) 2026 LingmoOS Team.
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

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import LingmoUI.CompatibleModule 3.0 as LingmoUI

// A notification's buttons and, when it supports it, the inline reply field
ColumnLayout {
    id: control

    property var actions: []            // [{key, label}]
    property bool hasReply: false
    property string replyPlaceholder: ""
    property string replySubmitText: ""
    readonly property bool replying: _replyRow.visible

    signal actionInvoked(string key)
    signal replied(string text)
    // The reply field opened: the popup has to take keyboard focus for it
    signal replyStarted()

    spacing: LingmoUI.Units.smallSpacing
    visible: (actions && actions.length > 0) || hasReply

    RowLayout {
        Layout.fillWidth: true
        visible: !_replyRow.visible
        spacing: LingmoUI.Units.smallSpacing

        Repeater {
            model: control.actions

            Button {
                Layout.fillWidth: true
                Layout.maximumWidth: implicitWidth + LingmoUI.Units.largeSpacing * 2
                text: modelData.label
                onClicked: control.actionInvoked(modelData.key)
            }
        }

        Button {
            visible: control.hasReply
            Layout.fillWidth: true
            Layout.maximumWidth: implicitWidth + LingmoUI.Units.largeSpacing * 2
            text: qsTr("Reply")
            icon.name: "mail-reply-sender"
            onClicked: {
                _replyRow.visible = true
                control.replyStarted()
                _replyField.forceActiveFocus()
            }
        }

        Item { Layout.fillWidth: true }
    }

    RowLayout {
        id: _replyRow
        Layout.fillWidth: true
        visible: false
        spacing: LingmoUI.Units.smallSpacing

        TextField {
            id: _replyField
            Layout.fillWidth: true
            placeholderText: control.replyPlaceholder || qsTr("Write a reply…")
            onAccepted: _sendButton.clicked()
            Keys.onEscapePressed: {
                text = ""
                _replyRow.visible = false
            }
        }

        Button {
            id: _sendButton
            text: control.replySubmitText || qsTr("Send")
            enabled: _replyField.text.trim() !== ""
            onClicked: {
                if (!enabled)
                    return
                control.replied(_replyField.text)
                _replyField.text = ""
                _replyRow.visible = false
            }
        }
    }
}
