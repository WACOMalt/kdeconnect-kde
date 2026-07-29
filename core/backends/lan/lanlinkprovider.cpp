/**
 * SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "lanlinkprovider.h"
#include "core_debug.h"

#include <memory>

#ifndef Q_OS_WIN
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#else
#include <winsock2.h>
// Winsock2 needs to be included before any other header
#include <mstcpip.h>
#endif

#include <QHostInfo>
#include <QMetaEnum>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSslKey>
#include <QStringList>
#include <QTcpServer>
#include <QUdpSocket>

#include "daemon.h"
#include "dbushelper.h"
#include "kdeconnectconfig.h"
#include "landevicelink.h"
#include "mdnshdiscovery.h"
#if !defined(Q_OS_WIN) && !defined(Q_OS_MAC)
#include "avahidiscovery.h"
#endif

// The size here is arbitrary, it needs to be considerably long as it includes the capabilities but there needs to be a limit.
// Tested between my systems and I get around 2000 per identity package.
static const int MAX_IDENTITY_PACKET_SIZE = 8192;

static const int MAX_UNPAIRED_CONNECTIONS = 42;

static const long MILLIS_DELAY_BETWEEN_CONNECTIONS_TO_SAME_DEVICE = 500;

LanLinkProvider::LanLinkProvider(bool testMode, bool isDisabled)
    : m_server(new Server(this))
    , m_udpSocket(this)
    , m_tcpPort(0)
    , m_testMode(testMode)
    , m_combineNetworkChangeTimer(this)
    , m_disabled(isDisabled)
    , m_directConnectTimer(this)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    // TODO: Both Windows and macOS have system APIs for mDNS that we could use.
    //       Windows docs: https://learn.microsoft.com/en-us/uwp/api/windows.networking.servicediscovery.dnssd
    m_mdnsDiscovery = new MdnshDiscovery(this);
#else
    if (AvahiDiscovery::hasAvahiDaemonRunning()) {
        qCDebug(KDECONNECT_CORE) << "Using Avahi for mDNS discovery";
        m_mdnsDiscovery = new AvahiDiscovery(this);
    } else {
        qCDebug(KDECONNECT_CORE) << "Using mdnsh for mDNS discovery";
        m_mdnsDiscovery = new MdnshDiscovery(this);
    }
#endif

    m_combineNetworkChangeTimer.setInterval(0); // increase this if waiting a single event-loop iteration is not enough
    m_combineNetworkChangeTimer.setSingleShot(true);
    connect(&m_combineNetworkChangeTimer, &QTimer::timeout, this, &LanLinkProvider::debouncedOnNetworkChange);

    connect(&m_udpSocket, &QIODevice::readyRead, this, &LanLinkProvider::udpBroadcastReceived);

    m_server->setProxy(QNetworkProxy::NoProxy);
    connect(m_server, &QTcpServer::newConnection, this, &LanLinkProvider::newTcpConnection);

    m_udpSocket.setProxy(QNetworkProxy::NoProxy);

    connect(&m_udpSocket, &QAbstractSocket::errorOccurred, nullptr, [](QAbstractSocket::SocketError socketError) {
        qWarning() << "Error sending UDP packet:" << socketError;
    });

    m_directConnectTimer.setInterval(DIRECT_CONNECT_INTERVAL_MS);
    m_directConnectTimer.setSingleShot(false);
    connect(&m_directConnectTimer, &QTimer::timeout, this, &LanLinkProvider::directConnectTimeout);

    const auto checkNetworkChange = [this]() {
        if (QNetworkInformation::instance()->reachability() == QNetworkInformation::Reachability::Online) {
            onNetworkChange();
        }
    };
    // Detect when a network interface changes status, so we announce ourselves in the new network
    QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Reachability);

    // We want to know if our current network reachability has changed, or if we change from one network to another
    connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this, checkNetworkChange);
    connect(QNetworkInformation::instance(), &QNetworkInformation::transportMediumChanged, this, checkNetworkChange);
}

LanLinkProvider::~LanLinkProvider()
{
    delete m_mdnsDiscovery;
}

void LanLinkProvider::enable()
{
    if (m_disabled == true) {
        m_disabled = false;
        onStart();
    }
}

void LanLinkProvider::disable()
{
    if (m_disabled == false) {
        onStop();
        m_disabled = true;
    }
}

void LanLinkProvider::onStart()
{
    if (m_disabled) {
        return;
    }

    const QHostAddress bindAddress = m_testMode ? QHostAddress::LocalHost : QHostAddress::Any;

    bool success = m_udpSocket.bind(bindAddress, UDP_PORT, QUdpSocket::ShareAddress);
    if (!success) {
        QAbstractSocket::SocketError sockErr = m_udpSocket.error();
        // Refer to https://doc.qt.io/qt-5/qabstractsocket.html#SocketError-enum to decode socket error number
        QString errorMessage = QString::fromLatin1(QMetaEnum::fromType<QAbstractSocket::SocketError>().valueToKey(sockErr));
        qCritical(KDECONNECT_CORE) << "Failed to bind UDP socket on port" << UDP_PORT << "with error" << errorMessage;
    }
    Q_ASSERT(success);

    m_tcpPort = MIN_TCP_PORT;
    while (!m_server->listen(bindAddress, m_tcpPort)) {
        m_tcpPort++;
        if (m_tcpPort > MAX_TCP_PORT) { // No ports available?
            qCritical(KDECONNECT_CORE) << "Error opening a port in range" << MIN_TCP_PORT << "-" << MAX_TCP_PORT;
            m_tcpPort = 0;
            return;
        }
    }

    broadcastUdpIdentityPacket();

    m_mdnsDiscovery->onStart();

    directConnectToDevices();
    m_directConnectTimer.start();

    qCDebug(KDECONNECT_CORE) << "LanLinkProvider started";
}

void LanLinkProvider::onStop()
{
    if (m_disabled) {
        return;
    }
    m_directConnectTimer.stop();
    for (auto it = m_pendingDirectConnections.begin(); it != m_pendingDirectConnections.end(); ++it) {
        it.value()->abort();
        it.value()->deleteLater();
    }
    m_pendingDirectConnections.clear();
    m_mdnsDiscovery->onStop();
    m_udpSocket.close();
    m_server->close();
    qCDebug(KDECONNECT_CORE) << "LanLinkProvider stopped";
}

void LanLinkProvider::onNetworkChange()
{
    if (m_disabled) {
        return;
    }
    if (m_combineNetworkChangeTimer.isActive()) {
        qCDebug(KDECONNECT_CORE) << "Device discovery triggered too fast, ignoring";
        return;
    }
    m_combineNetworkChangeTimer.start();
}

// I'm in a new network, let's be polite and introduce myself
void LanLinkProvider::debouncedOnNetworkChange()
{
    if (m_disabled) {
        return;
    }
    if (!m_server->isListening()) {
        qWarning() << "TCP server not listening, not broadcasting";
        return;
    }

    Q_ASSERT(m_tcpPort != 0);

    broadcastUdpIdentityPacket();
    m_mdnsDiscovery->onNetworkChange();
    directConnectToDevices();
}

void LanLinkProvider::broadcastUdpIdentityPacket()
{
    if (qEnvironmentVariableIsSet("KDECONNECT_DISABLE_UDP_BROADCAST")) {
        qWarning() << "Not broadcasting UDP because KDECONNECT_DISABLE_UDP_BROADCAST is set";
        return;
    }
    qCDebug(KDECONNECT_CORE) << "Broadcasting identity packet";

    sendUdpIdentityPacket(getBroadcastAddresses());
}

QList<QHostAddress> LanLinkProvider::getBroadcastAddresses()
{
    const QStringList customDevices = KdeConnectConfig::instance().customDevices();

    QList<QHostAddress> destinations;
    destinations.reserve(customDevices.length() + 1);

    destinations.append(m_testMode ? QHostAddress::LocalHost : QHostAddress::Broadcast);

    // Add custom device IPs as unicast UDP targets; hostnames are handled via direct TCP
    for (const auto &customDevice : customDevices) {
        QStringList hostPort = parseCustomDeviceHost(customDevice);
        QHostAddress address(hostPort[0]);
        if (!address.isNull()) {
            destinations.append(address);
        }
    }

    return destinations;
}

void LanLinkProvider::sendUdpIdentityPacket(const QList<QHostAddress> &addresses)
{
    QUdpSocket sendSocket;
    sendSocket.setProxy(QNetworkProxy::NoProxy);
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if ((iface.flags() & QNetworkInterface::IsUp) && (iface.flags() & QNetworkInterface::IsRunning)) {
            for (const QNetworkAddressEntry &ifaceAddress : iface.addressEntries()) {
                QHostAddress sourceAddress = ifaceAddress.ip();
                if (sourceAddress.protocol() == QAbstractSocket::IPv4Protocol && sourceAddress != QHostAddress::LocalHost) {
                    qCDebug(KDECONNECT_CORE) << "Broadcasting as" << sourceAddress << "on" << iface.name();
                    sendSocket.bind(sourceAddress);
                    sendUdpIdentityPacket(sendSocket, addresses);
                    sendSocket.close();
                }
            }
        }
    }
}

void LanLinkProvider::sendUdpIdentityPacket(QUdpSocket &socket, const QList<QHostAddress> &addresses)
{
    DeviceInfo myDeviceInfo = KdeConnectConfig::instance().deviceInfo();
    NetworkPacket identityPacket = myDeviceInfo.toIdentityPacket();
    identityPacket.set(QStringLiteral("tcpPort"), m_tcpPort);

    const QByteArray payload = identityPacket.serialize();

    for (auto &address : addresses) {
        qint64 bytes = socket.writeDatagram(payload, address, UDP_PORT);
        if (bytes == -1 && socket.error() == QAbstractSocket::DatagramTooLargeError) {
            // On macOS and FreeBSD, UDP broadcasts larger than MTU get dropped. See:
            // https://opensource.apple.com/source/xnu/xnu-3789.1.32/bsd/netinet/ip_output.c.auto.html
            // We remove the capabilities to reduce the size of the packet.
            qWarning() << "Identity packet to" << address << "got rejected because it was too large. Retrying without including the capabilities";
            identityPacket.set(QStringLiteral("outgoingCapabilities"), QStringList());
            identityPacket.set(QStringLiteral("incomingCapabilities"), QStringList());
            const QByteArray smallPayload = identityPacket.serialize();
            socket.writeDatagram(smallPayload, address, UDP_PORT);
        }
    }
}

// I'm the existing device, a new device is kindly introducing itself.
// I will create a TcpSocket and try to connect. This can result in either tcpSocketConnected() or connectError().
void LanLinkProvider::udpBroadcastReceived()
{
    while (m_udpSocket.hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_udpSocket.pendingDatagramSize());
        QHostAddress sender;

        m_udpSocket.readDatagram(datagram.data(), datagram.size(), &sender);

        if (sender.isLoopback() && !m_testMode)
            continue;

        std::shared_ptr<NetworkPacket> receivedPacket = std::shared_ptr<NetworkPacket>{new NetworkPacket()};
        bool success = NetworkPacket::unserialize(datagram, receivedPacket.get());

        // qCDebug(KDECONNECT_CORE) << "Datagram " << datagram.data() ;

        if (!success) {
            qCDebug(KDECONNECT_CORE) << "Could not unserialize UDP packet";
            continue;
        }

        if (!DeviceInfo::isValidIdentityPacket(receivedPacket.get())) {
            qCWarning(KDECONNECT_CORE) << "Invalid identity packet received";
            continue;
        }

        QString deviceId = receivedPacket->get<QString>(QStringLiteral("deviceId"));

        if (deviceId == KdeConnectConfig::instance().deviceId()) {
            // qCDebug(KDECONNECT_CORE) << "Ignoring my own broadcast";
            continue;
        }

        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastConnectionTime[deviceId] + MILLIS_DELAY_BETWEEN_CONNECTIONS_TO_SAME_DEVICE > now) {
            qCDebug(KDECONNECT_CORE) << "Discarding second UPD packet from the same device" << deviceId << "received too quickly";
            return;
        }
        m_lastConnectionTime[deviceId] = now;

        int tcpPort = receivedPacket->get<int>(QStringLiteral("tcpPort"));
        if (tcpPort < MIN_TCP_PORT || tcpPort > MAX_TCP_PORT) {
            qCDebug(KDECONNECT_CORE) << "TCP port outside of kdeconnect's range";
            continue;
        }

        bool isDeviceTrusted = KdeConnectConfig::instance().trustedDevices().contains(deviceId);
        int protocolVersion = receivedPacket->get<int>(QStringLiteral("protocolVersion"), 0);
        if (isDeviceTrusted && isProtocolDowngrade(deviceId, protocolVersion)) {
            qCWarning(KDECONNECT_CORE) << "Refusing to connect to a device using an older protocol version. Ignoring " << deviceId;
            return;
        }

        // qCDebug(KDECONNECT_CORE) << "Received Udp identity packet from" << sender << " asking for a tcp connection on port " << tcpPort;

        QSslSocket *socket = new QSslSocket(this);
        socket->setProxy(QNetworkProxy::NoProxy);
        connect(socket, &QAbstractSocket::errorOccurred, this, [this, socket, sender](QAbstractSocket::SocketError socketError) {
            connectError(socket, sender, socketError);
        });
        connect(socket, &QAbstractSocket::connected, this, [this, socket, receivedPacket, sender]() {
            tcpSocketConnected(socket, receivedPacket, sender);
        });
        socket->connectToHost(sender, tcpPort);
    }
}

void LanLinkProvider::connectError(QSslSocket *socket, QHostAddress sender, QAbstractSocket::SocketError socketError)
{
    qCDebug(KDECONNECT_CORE) << "Socket error" << socketError;
    qCDebug(KDECONNECT_CORE) << "Fallback (1), try reverse connection (send udp packet)" << socket->errorString();
    NetworkPacket np = KdeConnectConfig::instance().deviceInfo().toIdentityPacket();
    np.set(QStringLiteral("tcpPort"), m_tcpPort);
    np.set(QStringLiteral("outgoingCapabilities"), QStringList());
    np.set(QStringLiteral("incomingCapabilities"), QStringList());
    m_udpSocket.writeDatagram(np.serialize(), sender, UDP_PORT);

    // The socket we created didn't work, and we didn't manage
    // to create a LanDeviceLink from it, deleting everything.
    socket->deleteLater();
}

// We received a UDP packet and answered by connecting to them by TCP. This gets called on a successful connection.
// TODO: When we support protocol version 8 only, this method doesn't need to take a networkpacket, just deviceId and protocolVersion
void LanLinkProvider::tcpSocketConnected(QSslSocket *socket, std::shared_ptr<NetworkPacket> receivedPacket, QHostAddress sender)
{
    disconnect(socket, &QAbstractSocket::errorOccurred, this, nullptr);

    configureSocket(socket);

    // If socket disconnects due to any reason after connection, link on ssl failure
    connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);

    const QString &deviceId = receivedPacket->get<QString>(QStringLiteral("deviceId"));
    const int protocolVersion = receivedPacket->get<int>(QStringLiteral("protocolVersion"));

    NetworkPacket np2 = KdeConnectConfig::instance().deviceInfo().toIdentityPacket();
    np2.set(QStringLiteral("targetDeviceId"), deviceId);
    np2.set(QStringLiteral("targetProtocolVersion"), protocolVersion);
    socket->write(np2.serialize());
    bool success = socket->waitForBytesWritten();

    if (success) {
        qCDebug(KDECONNECT_CORE) << "TCP connection done (i'm the existing device)";

        // if ssl supported
        bool isDeviceTrusted = KdeConnectConfig::instance().trustedDevices().contains(deviceId);
        configureSslSocket(socket, deviceId, isDeviceTrusted);

        qCDebug(KDECONNECT_CORE) << "Starting server ssl (I'm the client TCP socket)";

        connect(socket, &QSslSocket::encrypted, this, [this, socket, receivedPacket]() {
            encrypted(socket, receivedPacket);
        });

        connect(socket, &QSslSocket::sslErrors, this, &LanLinkProvider::sslErrors);

        socket->startServerEncryption();
    } else {
        // The socket doesn't seem to work, so we can't create the connection.

        qCDebug(KDECONNECT_CORE) << "Fallback (2), try reverse connection (send udp packet)";
        np2.set(QStringLiteral("outgoingCapabilities"), QStringList());
        np2.set(QStringLiteral("incomingCapabilities"), QStringList());
        m_udpSocket.writeDatagram(np2.serialize(), sender, UDP_PORT);

        // Disconnect should trigger deleteLater
        socket->abort();
    }
}

void LanLinkProvider::encrypted(QSslSocket *socket, std::shared_ptr<NetworkPacket> identityPacket)
{
    qCDebug(KDECONNECT_CORE) << "Socket successfully established an SSL connection";

    Q_ASSERT(socket->mode() != QSslSocket::UnencryptedMode);

    QString deviceId = identityPacket->get<QString>(QStringLiteral("deviceId"));
    int protocolVersion = identityPacket->get<int>(QStringLiteral("protocolVersion"), -1);
    if (protocolVersion >= 8) {
        NetworkPacket myIdentity = KdeConnectConfig::instance().deviceInfo().toIdentityPacket();
        socket->write(myIdentity.serialize());
        socket->flush();
        connect(socket, &QIODevice::readyRead, this, [this, socket, protocolVersion, deviceId]() {
            if (socket->bytesAvailable() > MAX_IDENTITY_PACKET_SIZE) {
                qCWarning(KDECONNECT_CORE) << "Remote device sent a packet too large";
                socket->abort();
                return;
            }
            if (!socket->canReadLine()) {
                // This can happen if the packet is large enough to be split in two chunks
                return;
            }
            disconnect(socket, &QIODevice::readyRead, nullptr, nullptr);
            QByteArray identityString = socket->readLine();
            NetworkPacket secureIdentityPacket;
            bool success = NetworkPacket::unserialize(identityString, &secureIdentityPacket);
            if (!success || !DeviceInfo::isValidIdentityPacket(&secureIdentityPacket)) {
                qCWarning(KDECONNECT_CORE) << "Remote device doesn't correctly implement protocol version 8";
                socket->abort();
                return;
            }
            int newProtocolVersion = secureIdentityPacket.get<int>(QStringLiteral("protocolVersion"), 0);
            if (newProtocolVersion != protocolVersion) {
                qCWarning(KDECONNECT_CORE) << "Protocol version changed half-way through the handshake:" << protocolVersion << "->" << newProtocolVersion;
                socket->abort();
                return;
            }
            QString newDeviceId = secureIdentityPacket.get<QString>(QStringLiteral("deviceId"));
            if (newDeviceId != deviceId) {
                qCWarning(KDECONNECT_CORE) << "Device ID changed half-way through the handshake:" << deviceId << "->" << newDeviceId;
                socket->abort();
                return;
            }
            DeviceInfo deviceInfo = DeviceInfo::FromIdentityPacketAndCert(secureIdentityPacket, socket->peerCertificate());

            addLink(socket, deviceInfo);
        });
    } else {
        DeviceInfo deviceInfo = DeviceInfo::FromIdentityPacketAndCert(*identityPacket, socket->peerCertificate());
        addLink(socket, deviceInfo);
    }
}

void LanLinkProvider::sslErrors(const QList<QSslError> &errors)
{
    bool fatal = false;
    for (const QSslError &error : errors) {
        if (error.error() != QSslError::SelfSignedCertificate) {
            qCCritical(KDECONNECT_CORE) << "Disconnecting due to fatal SSL Error: " << error;
            fatal = true;
        } else {
            qCDebug(KDECONNECT_CORE) << "Ignoring self-signed cert error";
        }
    }

    if (fatal) {
        QSslSocket *socket = qobject_cast<QSslSocket *>(sender());
        if (socket) {
            // Disconnect should trigger deleteLater
            socket->abort();
        }
    }
}

// I'm the new device and this is the answer to my UDP identity packet (no data received yet). They are connecting to us through TCP, and they should send an
// identity.
void LanLinkProvider::newTcpConnection()
{
    qCDebug(KDECONNECT_CORE) << "LanLinkProvider newTcpConnection";

    while (m_server->hasPendingConnections()) {
        QSslSocket *socket = m_server->nextPendingConnection();
        configureSocket(socket);
        // This socket is still managed by us (and child of the QTcpServer), if
        // it disconnects before we manage to pass it to a LanDeviceLink, it's
        // our responsibility to delete it. We do so with this connection.
        connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QIODevice::readyRead, this, &LanLinkProvider::tcpPacketReceived);

        QTimer *timer = new QTimer(socket);
        timer->setSingleShot(true);
        timer->setInterval(1000);
        connect(socket, &QSslSocket::encrypted, timer, &QObject::deleteLater);
        connect(timer, &QTimer::timeout, socket, [socket] {
            qCWarning(KDECONNECT_CORE) << "LanLinkProvider/newTcpConnection: Host timed out without sending any identity." << socket->peerAddress();
            socket->abort();
        });
        timer->start();
    }
}

// I'm the new device and this is the TCP response to my UDP identity packet
void LanLinkProvider::tcpPacketReceived()
{
    QSslSocket *socket = qobject_cast<QSslSocket *>(sender());
    if (socket->bytesAvailable() > MAX_IDENTITY_PACKET_SIZE) {
        qCWarning(KDECONNECT_CORE) << "LanLinkProvider/newTcpConnection: Suspiciously long identity package received. Closing connection."
                                   << socket->peerAddress() << socket->bytesAvailable();
        socket->abort();
        return;
    }

    if (!socket->canReadLine()) {
        // This can happen if the packet is large enough to be split in two chunks
        return;
    }

    const QByteArray data = socket->readLine();

    qCDebug(KDECONNECT_CORE) << "LanLinkProvider received reply:" << data;

    std::shared_ptr<NetworkPacket> np = std::shared_ptr<NetworkPacket>{new NetworkPacket()};
    bool success = NetworkPacket::unserialize(data, np.get());

    if (!success) {
        return;
    }

    if (!DeviceInfo::isValidIdentityPacket(np.get())) {
        qCWarning(KDECONNECT_CORE) << "Invalid identity packet received";
        return;
    }

    QString targetDeviceId = np->get<QString>(QStringLiteral("targetDeviceId"));
    int targetProtocolVersion = np->get<int>(QStringLiteral("targetProtocolVersion"), -1);
    if (!targetDeviceId.isEmpty() && targetDeviceId != KdeConnectConfig::instance().deviceId()) {
        qCWarning(KDECONNECT_CORE) << "Received a connection request for a device that isn't me:" << targetDeviceId;
        return;
    }
    if (targetProtocolVersion != -1 && targetProtocolVersion != NetworkPacket::s_protocolVersion) {
        qCWarning(KDECONNECT_CORE) << "Received a connection request for a protocol version that isn't mine:" << targetProtocolVersion;
        return;
    }

    const QString &deviceId = np->get<QString>(QStringLiteral("deviceId"));

    bool isDeviceTrusted = KdeConnectConfig::instance().trustedDevices().contains(deviceId);
    int protocolVersion = np->get<int>(QStringLiteral("protocolVersion"), 0);
    if (isDeviceTrusted && isProtocolDowngrade(deviceId, protocolVersion)) {
        qCWarning(KDECONNECT_CORE) << "Refusing to connect to a device using an older protocol version" << protocolVersion << ". Ignoring" << deviceId;
        return;
    }

    // qCDebug(KDECONNECT_CORE) << "Handshaking done (i'm the new device)";

    // This socket will now be owned by the LanDeviceLink or we don't want more data to be received, forget about it
    disconnect(socket, &QIODevice::readyRead, this, &LanLinkProvider::tcpPacketReceived);

    configureSslSocket(socket, deviceId, isDeviceTrusted);

    qCDebug(KDECONNECT_CORE) << "Starting client ssl (but I'm the server TCP socket)";

    connect(socket, &QSslSocket::encrypted, this, [this, socket, np]() {
        encrypted(socket, np);
    });

    if (isDeviceTrusted) {
        connect(socket, &QSslSocket::sslErrors, this, &LanLinkProvider::sslErrors);
    }

    socket->startClientEncryption(); // Will call encrypted()
}

bool LanLinkProvider::isProtocolDowngrade(const QString &deviceId, int protocolVersion) const
{
    int lastKnownProtocolVersion = KdeConnectConfig::instance().getTrustedDeviceProtocolVersion(deviceId);
    return lastKnownProtocolVersion > protocolVersion;
}

void LanLinkProvider::onLinkDestroyed(const QString &deviceId, DeviceLink *oldPtr)
{
    qCDebug(KDECONNECT_CORE) << "LanLinkProvider deviceLinkDestroyed" << deviceId;
    DeviceLink *link = m_links.take(deviceId);
    Q_ASSERT(link == oldPtr);

    if (KdeConnectConfig::instance().trustedDevices().contains(deviceId)) {
        qCDebug(KDECONNECT_CORE) << "Trusted device link lost, scheduling reconnection for" << deviceId;
        broadcastUdpIdentityPacket();
        QTimer::singleShot(DIRECT_CONNECT_RETRY_DELAY_MS, this, [this]() {
            directConnectToDevices();
        });
    }
}

void LanLinkProvider::configureSslSocket(QSslSocket *socket, const QString &deviceId, bool isDeviceTrusted)
{
    // Configure for ssl
    QSslConfiguration sslConfig;
    sslConfig.setLocalCertificate(KdeConnectConfig::instance().certificate());
    sslConfig.setPrivateKey(KdeConnectConfig::instance().privateKey());

    if (isDeviceTrusted) {
        QSslCertificate certificate = KdeConnectConfig::instance().getTrustedDeviceCertificate(deviceId);
        sslConfig.setCaCertificates({certificate});
        sslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);
    } else {
        sslConfig.setPeerVerifyMode(QSslSocket::QueryPeer);
    }
    socket->setSslConfiguration(sslConfig);
    socket->setPeerVerifyName(deviceId);

    // Usually SSL errors are only bad for trusted devices. Uncomment this section to log errors in any case, for debugging.
    // connect(socket, &QSslSocket::sslErrors, [](const QList<QSslError>& errors)
    // {
    //      for (const QSslError& error : errors) {
    //          qCDebug(KDECONNECT_CORE) << "SSL Error:" << error.errorString();
    //      }
    // });
}

void LanLinkProvider::configureSocket(QSslSocket *socket)
{
    socket->setProxy(QNetworkProxy::NoProxy);
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, QVariant(1));

    int fd = socket->socketDescriptor();
    if (fd >= 0) {
#ifdef TCP_KEEPIDLE
        // Linux: Detect dead connections in ~60s instead of the default ~7875s.
        // Without this, a phone that silently disappears (e.g. WiFi off) leaves a
        // stale connection where isReachable() remains true and sends fail without
        // any user-visible error (BUG 476747).
        int idle = 30; // seconds before first keepalive probe
        int intvl = 10; // seconds between probes
        int cnt = 3; // failed probes before connection is dropped
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#elif defined(Q_OS_MAC)
// macOS: TCP_KEEPALIVE is the equivalent of Linux's TCP_KEEPIDLE
// TCP_KEEPINTVL and TCP_KEEPCNT are available on macOS 10.8+
#ifndef TCP_KEEPALIVE
#define TCP_KEEPALIVE 0x10
#endif
        int idle = 30;
        int intvl = 10;
        int cnt = 3;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
    }
}

void LanLinkProvider::addLink(QSslSocket *socket, const DeviceInfo &deviceInfo)
{
    QString certDeviceId = socket->peerCertificate().subjectDisplayName();
    DBusHelper::filterNonExportableCharacters(certDeviceId);
    if (deviceInfo.id != certDeviceId) {
        socket->abort();
        qCWarning(KDECONNECT_CORE) << "DeviceID in cert doesn't match deviceID in identity packet." << deviceInfo.id << "vs" << certDeviceId;
        return;
    }

    // Socket disconnection will now be handled by LanDeviceLink
    disconnect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);

    LanDeviceLink *deviceLink;
    // Do we have a link for this device already?
    QMap<QString, LanDeviceLink *>::iterator linkIterator = m_links.find(deviceInfo.id);
    if (linkIterator != m_links.end()) {
        deviceLink = linkIterator.value();
        if (deviceLink->deviceInfo().certificate != deviceInfo.certificate) {
            qWarning() << "LanLink was asked to replace a socket but the certificate doesn't match, aborting";
            return;
        }
        // qCDebug(KDECONNECT_CORE) << "Reusing link to" << deviceId;
        deviceLink->reset(socket);
    } else {
        deviceLink = new LanDeviceLink(deviceInfo, this, socket);
        // Socket disconnection will now be handled by LanDeviceLink
        disconnect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);
        bool isDeviceTrusted = KdeConnectConfig::instance().trustedDevices().contains(deviceInfo.id);
        if (!isDeviceTrusted && m_links.size() > MAX_UNPAIRED_CONNECTIONS) {
            qCWarning(KDECONNECT_CORE) << "Too many unpaired devices to remember them all. Ignoring" << deviceInfo.id;
            socket->abort();
            socket->deleteLater();
            return;
        }
        m_links[deviceInfo.id] = deviceLink;
    }
    Q_EMIT onConnectionReceived(deviceLink);
}

QStringList LanLinkProvider::parseCustomDeviceHost(const QString &entry) const
{
    QString host = entry;
    QString port = QString::number(MIN_TCP_PORT);

    if (host.startsWith(QLatin1Char('['))) {
        int closingBracket = host.indexOf(QLatin1Char(']'));
        if (closingBracket >= 0) {
            if (closingBracket + 1 < host.size() && host[closingBracket + 1] == QLatin1Char(':')) {
                port = host.mid(closingBracket + 2);
            }
            host = host.mid(1, closingBracket - 1);
        }
    } else {
        int lastColon = host.lastIndexOf(QLatin1Char(':'));
        if (lastColon >= 0) {
            QString possiblePort = host.mid(lastColon + 1);
            bool ok;
            int portNum = possiblePort.toInt(&ok);
            if (ok && portNum >= MIN_TCP_PORT && portNum <= MAX_TCP_PORT) {
                host = host.left(lastColon);
                port = possiblePort;
            }
        }
    }
    return {host, port};
}

void LanLinkProvider::directConnectTimeout()
{
    broadcastUdpIdentityPacket();
    directConnectToDevices();
}

void LanLinkProvider::directConnectToDevices()
{
    if (m_disabled || m_tcpPort == 0) {
        return;
    }

    const QStringList customDevices = KdeConnectConfig::instance().customDevices();
    if (customDevices.isEmpty()) {
        return;
    }

    qCDebug(KDECONNECT_CORE) << "Attempting direct connections to custom devices";

    for (const QString &entry : customDevices) {
        QStringList hostPort = parseCustomDeviceHost(entry);
        QString host = hostPort[0];
        quint16 port = hostPort[1].toUShort();

        // Check if it's a plain IP address
        QHostAddress address(host);
        if (!address.isNull()) {
            directConnectToHost(address, port);
        } else {
            // It's a hostname — resolve asynchronously
            QString key = host + QLatin1Char(':') + QString::number(port);
            if (m_resolvingHosts.contains(key)) {
                continue; // Already resolving this host
            }
            m_resolvingHosts.insert(key);
            QHostInfo::lookupHost(host, this, [this, key, port](const QHostInfo &hostInfo) {
                m_resolvingHosts.remove(key);
                directHostResolved(hostInfo, port);
            });
        }
    }
}

void LanLinkProvider::directHostResolved(const QHostInfo &hostInfo, quint16 port)
{
    if (hostInfo.error() != QHostInfo::NoError) {
        qCDebug(KDECONNECT_CORE) << "Could not resolve custom device host:" << hostInfo.hostName() << "-" << hostInfo.errorString();
        return;
    }

    if (hostInfo.addresses().isEmpty()) {
        qCDebug(KDECONNECT_CORE) << "No addresses found for custom device host:" << hostInfo.hostName();
        return;
    }

    const QHostAddress address = hostInfo.addresses().constFirst();
    qCDebug(KDECONNECT_CORE) << "Resolved custom device host" << hostInfo.hostName() << "to" << address;

    directConnectToHost(address, port);
}

void LanLinkProvider::directConnectToHost(const QHostAddress &address, quint16 port)
{
    if (address.isLoopback() && !m_testMode) {
        return;
    }

    for (auto it = m_links.constBegin(); it != m_links.constEnd(); ++it) {
        if (it.value()->hostAddress() == address) {
            return;
        }
    }

    QString addrKey = address.toString() + QLatin1Char(':') + QString::number(port);
    if (m_pendingDirectConnections.contains(addrKey)) {
        return;
    }

    qCDebug(KDECONNECT_CORE) << "Direct TCP connect to" << address << ":" << port;

    QSslSocket *socket = new QSslSocket(this);
    socket->setProxy(QNetworkProxy::NoProxy);

    m_pendingDirectConnections.insert(addrKey, socket);

    QTimer *timeoutTimer = new QTimer(socket);
    timeoutTimer->setSingleShot(true);
    timeoutTimer->setInterval(10000); // 10 seconds

    connect(socket, &QAbstractSocket::connected, this, [this, socket, addrKey, timeoutTimer]() {
        timeoutTimer->stop();
        m_pendingDirectConnections.remove(addrKey);
        directTcpConnected(socket);
    });

    connect(socket, &QAbstractSocket::errorOccurred, this, [this, socket, addrKey, timeoutTimer](QAbstractSocket::SocketError) {
        timeoutTimer->stop();
        m_pendingDirectConnections.remove(addrKey);
        directTcpError(socket->error());
        socket->deleteLater();
    });

    connect(timeoutTimer, &QTimer::timeout, socket, [socket]() {
        socket->abort();
    });
    timeoutTimer->start();

    socket->connectToHost(address, port);
}

void LanLinkProvider::directTcpConnected(QSslSocket *socket)
{
    qCDebug(KDECONNECT_CORE) << "Direct TCP connected to" << socket->peerAddress();

    disconnect(socket, &QAbstractSocket::errorOccurred, this, nullptr);
    configureSocket(socket);

    connect(socket, &QAbstractSocket::disconnected, socket, &QObject::deleteLater);

    // Send identity to the remote's TCP server, same as the UDP-initiated flow
    NetworkPacket identityPacket = KdeConnectConfig::instance().deviceInfo().toIdentityPacket();
    identityPacket.set(QStringLiteral("tcpPort"), m_tcpPort);
    socket->write(identityPacket.serialize());
    bool success = socket->waitForBytesWritten();

    if (!success) {
        qCDebug(KDECONNECT_CORE) << "Direct connect: failed to write identity to" << socket->peerAddress();
        socket->abort();
        return;
    }
    QSslConfiguration sslConfig;
    sslConfig.setLocalCertificate(KdeConnectConfig::instance().certificate());
    sslConfig.setPrivateKey(KdeConnectConfig::instance().privateKey());
    sslConfig.setPeerVerifyMode(QSslSocket::QueryPeer);
    socket->setSslConfiguration(sslConfig);

    connect(socket, &QSslSocket::encrypted, this, [this, socket]() {
        qCDebug(KDECONNECT_CORE) << "Direct connect: SSL completed with" << socket->peerAddress();

        QString certDeviceId = socket->peerCertificate().subjectDisplayName();
        DBusHelper::filterNonExportableCharacters(certDeviceId);

        if (certDeviceId.isEmpty()) {
            qCWarning(KDECONNECT_CORE) << "Direct connect: peer certificate has no device ID";
            socket->abort();
            return;
        }

        if (certDeviceId == KdeConnectConfig::instance().deviceId()) {
            qCDebug(KDECONNECT_CORE) << "Direct connect: connected to ourselves, ignoring";
            socket->abort();
            return;
        }

        bool isDeviceTrusted = KdeConnectConfig::instance().trustedDevices().contains(certDeviceId);

        NetworkPacket myIdentity = KdeConnectConfig::instance().deviceInfo().toIdentityPacket();
        socket->write(myIdentity.serialize());
        socket->flush();

        QTimer *identityTimer = new QTimer(socket);
        identityTimer->setSingleShot(true);
        identityTimer->setInterval(5000);

        connect(socket, &QIODevice::readyRead, this, [this, socket, certDeviceId, isDeviceTrusted, identityTimer]() {
            if (socket->bytesAvailable() > MAX_IDENTITY_PACKET_SIZE) {
                identityTimer->stop();
                identityTimer->deleteLater();
                qCWarning(KDECONNECT_CORE) << "Direct connect: too much data from" << socket->peerAddress();
                socket->abort();
                return;
            }

            if (!socket->canReadLine()) {
                return; // Partial data, wait for more
            }

            identityTimer->stop();
            identityTimer->deleteLater();

            disconnect(socket, &QIODevice::readyRead, this, nullptr);

            QByteArray identityString = socket->readLine();
            NetworkPacket secureIdentityPacket;
            bool success = NetworkPacket::unserialize(identityString, &secureIdentityPacket);

            if (!success || !DeviceInfo::isValidIdentityPacket(&secureIdentityPacket)) {
                qCWarning(KDECONNECT_CORE) << "Direct connect: invalid secure identity from" << certDeviceId;
                socket->abort();
                return;
            }

            QString secureDeviceId = secureIdentityPacket.get<QString>(QStringLiteral("deviceId"));
            if (secureDeviceId != certDeviceId) {
                qCWarning(KDECONNECT_CORE) << "Direct connect: device ID mismatch:" << secureDeviceId << "vs" << certDeviceId;
                socket->abort();
                return;
            }

            int protocolVersion = secureIdentityPacket.get<int>(QStringLiteral("protocolVersion"), 0);
            if (isDeviceTrusted && isProtocolDowngrade(certDeviceId, protocolVersion)) {
                qCWarning(KDECONNECT_CORE) << "Direct connect: protocol downgrade from" << certDeviceId;
                socket->abort();
                return;
            }

            DeviceInfo deviceInfo = DeviceInfo::FromIdentityPacketAndCert(secureIdentityPacket, socket->peerCertificate());
            addLink(socket, deviceInfo);
        });

        connect(identityTimer, &QTimer::timeout, socket, [socket]() {
            qCWarning(KDECONNECT_CORE) << "Direct connect: timed out waiting for secure identity from" << socket->peerAddress();
            socket->abort();
        });

        identityTimer->start();
    });

    connect(socket, &QSslSocket::sslErrors, this, &LanLinkProvider::sslErrors);

    socket->startServerEncryption();
}

void LanLinkProvider::directTcpError(QAbstractSocket::SocketError socketError)
{
    qCDebug(KDECONNECT_CORE) << "Direct connect error:" << socketError;
}

#include "moc_lanlinkprovider.cpp"
