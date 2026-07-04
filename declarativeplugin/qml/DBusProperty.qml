/**
 * SPDX-FileCopyrightText: 2016 Aleix Pol Gonzalez <aleixpol@kde.org>
 * SPDX-FileCopyrightText: 2024 ivan tkachenko <me@ratijas.tk>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

pragma ComponentBehavior: Bound

import QtQml

import org.kde.kdeconnect as KDEConnect

QtObject {
    id: prop

    property QtObject object
    property string read
    property string change: read + "Changed"

    Component.onCompleted: {
        get();
    }

    // Holds a reference to the signal we last connected to.
    // Whenever `object` or `change` is rebound, the previous
    // signal handler must be disconnected so we don't keep
    // firing into a stale `value`, and the JS callback graph
    // doesn't grow over the component's lifetime.
    //
    // (Qt 6 QML's signal.connect(target, slot) returns
    // `undefined`, so a connection-handle pattern is a no-op;
    // disconnecting has to be done against the signal
    // reference itself, via signal.disconnect(target, slot).)
    property var _connectedSignal: null

    onChangeChanged: setupConnection()
    onObjectChanged: setupConnection()

    function setupConnection() {
        disconnectPrevious();
        // `read` is part of the guard because `onObjectChanged`
        // fires before the consumer has set `read`, leaving
        // `change` defaulted to just "Changed" — which doesn't
        // resolve on real D-Bus objects and would otherwise
        // produce a spurious console.warn on every Settings open.
        if (object && change && read) {
            const signal = object[change];
            if (signal) {
                signal.connect(this, valueReceived);
                _connectedSignal = signal;
            } else {
                console.warn(`couldn't find signal ${change} for ${object}`);
            }
        }
    }

    function disconnectPrevious() {
        if (_connectedSignal) {
            try {
                _connectedSignal.disconnect(this, valueReceived);
            } catch (e) {
                // Object may already be torn down — safe to swallow.
            }
            _connectedSignal = null;
        }
    }

    Component.onDestruction: disconnectPrevious()

    function valueReceived(value: var): void {
        if (!value) {
            get();
        } else {
            _value = value;
        }
    }

    property var defaultValue
    property var _value: defaultValue
    readonly property var value: _value

    readonly property KDEConnect.DBusAsyncResponse __response: KDEConnect.DBusAsyncResponse {
        id: response

        autoDelete: false

        onSuccess: result => {
            prop._value = result;
        }

        onError: message => {
            console.warn("failed call", prop.object, prop.read, prop.change, message);
        }
    }

    function get(): void {
        if (object) {
            const method = object[read];
            if (method) {
                response.setPendingCall(method());
            }
        }
    }
}
