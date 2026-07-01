// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.kdeconnect
import org.kde.kdeconnect.app

Kirigami.ScrollablePage
{
    id: page
    title: i18nc("@title:window", "Settings")

    // The daemon owns the canonical `customDevices` QStringList
    // (Daemon::setCustomDevices persists to ~/.config/kdeconnect/config).
    // We bind the Repeater directly to that property rather than
    // mirroring it into a local ListModel — the QML binding system
    // re-evaluates the model whenever the Q_PROPERTY's NOTIFY signal
    // fires, so we don't need (or want) a Connections block to keep
    // a mirror in sync. This avoids a feedback/echo loop that
    // occurred with the previous mirror-based design: the user's
    // write would clobber the local model mid-Repeater-creation.
    function addCustomDevice(address) {
        const trimmed = address.trim();
        if (trimmed.length === 0) {
            return;
        }
        const arr = DaemonDbusInterface.customDevices.slice();
        if (arr.indexOf(trimmed) >= 0) {
            return; // already present; silently ignore duplicates
        }
        arr.push(trimmed);
        DaemonDbusInterface.customDevices = arr;
    }

    function removeCustomDevice(index) {
        page.forceActiveFocus();
        const arr = DaemonDbusInterface.customDevices.slice();
        arr.splice(index, 1);
        DaemonDbusInterface.customDevices = arr;
    }

    ColumnLayout
    {

        FormCard.FormCard {
            Layout.topMargin: Kirigami.Units.gridUnit

            FormCard.FormTextFieldDelegate {
                id: deviceNameField
                text: announcedNameProperty.value
                onAccepted: DaemonDbusInterface.setAnnouncedName(text);
                label: i18n("Device name")

                DBusProperty {
                    id: announcedNameProperty
                    object: DaemonDbusInterface
                    read: "announcedName"
                    defaultValue: ""
                }
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Backends")
        }

        FormCard.FormCard {
            DBusProperty {
                id: linkProvidersProperty
                object: DaemonDbusInterface
                read: "linkProviders"
                defaultValue: []
            }
            visible: linkProvidersProperty.value.length > 0

            Repeater {
                model: linkProvidersProperty.value

                FormCard.FormCheckDelegate {
                    required property string modelData

                    readonly property string displayName: modelData.split('|')[0]
                    readonly property string internalName: modelData.split('|')[1]

                    checked: modelData.split('|')[2] === 'enabled'
                    text: displayName

                    onToggled: DaemonDbusInterface.setLinkProviderState(internalName, checked);
                }
            }
        }

        FormCard.FormHeader {
            title: i18nc("@title:group", "Custom Devices")
        }

        FormCard.FormCard {
            Layout.fillWidth: true

            FormCard.FormTextDelegate {
                Layout.fillWidth: true
                text: i18nc("@info",
                    "Add devices by IP or hostname to connect directly over LAN or VPN.")
                visible: DaemonDbusInterface.customDevices.length === 0
            }

            Repeater {
                model: DaemonDbusInterface.customDevices

                delegate: FormCard.AbstractFormDelegate {
                    id: customDeviceDelegate
                    required property int index
                    required property string modelData

                    contentItem: RowLayout {
                        Kirigami.Icon {
                            source: "network-server"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                        }
                        Label {
                            text: customDeviceDelegate.modelData
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        ToolButton {
                            icon.name: "edit-delete-remove"
                            text: i18nc("@action:button", "Remove")
                            display: AbstractButton.IconOnly
                            onClicked: Qt.callLater(removeCustomDevice, customDeviceDelegate.index)
                            ToolTip.text: text
                            ToolTip.visible: hovered
                            ToolTip.delay: Kirigami.Units.toolTipDelay
                        }
                    }
                }
            }

            FormCard.FormTextFieldDelegate {
                id: newDeviceField
                Layout.fillWidth: true
                label: i18nc("@label:textbox", "Add device")
                placeholderText: i18nc("@placeholder", "IP or hostname (e.g. 100.64.0.2)")
                onAccepted: {
                    if (text.trim().length > 0) {
                        addCustomDevice(text);
                        text = "";
                    }
                }
            }

            FormCard.FormButtonDelegate {
                text: i18nc("@action:button", "Add")
                icon.name: "list-add"
                onClicked: {
                    if (newDeviceField.text.trim().length > 0) {
                        addCustomDevice(newDeviceField.text);
                        newDeviceField.text = "";
                    }
                }
            }
        }

        FormCard.FormCard {
            Layout.topMargin: Kirigami.Units.gridUnit

            FormCard.FormButtonDelegate {
                text: i18n("About KDE Connect")
                onClicked: applicationWindow().pageStack.layers.push(Qt.createComponent("org.kde.kirigamiaddons.formcard", "AboutPage"))
                icon.name: 'kdeconnect'
            }

            FormCard.FormDelegateSeparator {}

            FormCard.FormButtonDelegate {
                text: i18n("About KDE")
                onClicked: applicationWindow().pageStack.layers.push(Qt.createComponent("org.kde.kirigamiaddons.formcard", "AboutKDEPage"))
                icon.name: 'kde'
            }
        }
    }

    footer: ToolBar {
        contentItem: RowLayout {
            Item { Layout.fillWidth: true }
            Button {
                text: i18n("Close")
                display: AbstractButton.TextOnly
                onClicked: {
                    DaemonDbusInterface.setAnnouncedName(deviceNameField.text);
                    page.Kirigami.PageStack.closeDialog();
                }
            }
        }
    }
}
