/****************************************************************************
**
** Copyright (C) 2015 Alexander Rössler
** License: LGPL version 2.1
**
** This file is part of QtQuickVcp.
**
** All rights reserved. This program and the accompanying materials
** are made available under the terms of the GNU Lesser General Public License
** (LGPL) version 2.1 which accompanies this distribution, and is available at
** http://www.gnu.org/licenses/lgpl-2.1.html
**
** This library is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
** Lesser General Public License for more details.
**
** Contributors:
** Alexander Rössler @ The Cool Tool GmbH <mail DOT aroessler AT gmail DOT com>
**
****************************************************************************/
#ifndef APPLICATIONLAUNCHER_H
#define APPLICATIONLAUNCHER_H

#include <abstractserviceimplementation.h>
#include <machinetalkservice.h>
#include <QJsonValue>
#include <nzmqt/nzmqt.hpp>
#include <google/protobuf/text_format.h>
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <machinetalk/protobuf/message.pb.h>
#include <machinetalk/protobuf/config.pb.h>

namespace qtquickvcp {

class ApplicationLauncher : public AbstractServiceImplementation
{
    Q_OBJECT
    Q_PROPERTY(QString launchercmdUri READ launchercmdUri WRITE setLaunchercmdUri NOTIFY launchercmdUriChanged)
    Q_PROPERTY(QString launcherUri READ launcherUri WRITE setLauncherUri NOTIFY launcherUriChanged)
    Q_PROPERTY(int heartbeatPeriod READ heartbeatPeriod WRITE heartbeatPeriod NOTIFY heartbeatPeriodChanged)
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(MachinetalkService::State connectionState READ connectionState NOTIFY connectionStateChanged)
    Q_PROPERTY(MachinetalkService::ConnectionError error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(QJsonValue launchers READ launchers NOTIFY launchersChanged)
    Q_PROPERTY(bool synced READ isSynced NOTIFY syncedChanged)

public:
    explicit ApplicationLauncher(QObject *parent = 0);
    ~ApplicationLauncher();

    QString launchercmdUri() const
    {
        return m_commandUri;
    }

    QString launcherUri() const
    {
        return m_subscribeUri;
    }

    int heartbeatPeriod() const
    {
        return m_heartbeatPeriod;
    }

    bool isConnected() const
    {
        return m_connected;
    }

    MachinetalkService::State connectionState() const
    {
        return m_connectionState;
    }

    MachinetalkService::ConnectionError error() const
    {
        return m_error;
    }

    QString errorString() const
    {
        return m_errorString;
    }

    QJsonValue launchers() const
    {
        return m_launchers;
    }

    bool isSynced() const
    {
        return m_synced;
    }

public slots:
    void setLaunchercmdUri(QString arg)
    {
        if (m_commandUri == arg)
            return;

        m_commandUri = arg;
        emit launchercmdUriChanged(arg);
    }

    void setLauncherUri(QString arg)
    {
        if (m_subscribeUri == arg)
            return;

        m_subscribeUri = arg;
        emit launcherUriChanged(arg);
    }

    void heartbeatPeriod(int arg)
    {
        if (m_heartbeatPeriod == arg)
            return;

        m_heartbeatPeriod = arg;
        emit heartbeatPeriodChanged(arg);
    }

    void start(int index);
    void terminate(int index);
    void kill(int index);
    void writeToStdin(int index, const QString &data);
    void call(const QString &command);
    void shutdown();

private:
    QString m_subscribeUri;
    QString m_commandUri;
    QString m_commandIdentity;
    int m_heartbeatPeriod;
    bool m_connected;
    MachinetalkService::SocketState m_subscribeSocketState;
    MachinetalkService::SocketState m_commandSocketState;
    MachinetalkService::State m_connectionState;
    MachinetalkService::ConnectionError m_error;
    QString m_errorString;
    QJsonValue m_launchers;
    bool m_synced;

    nzmqt::PollingZMQContext *m_context;
    nzmqt::ZMQSocket  *m_subscribeSocket;
    nzmqt::ZMQSocket  *m_commandSocket;
    QTimer     *m_commandHeartbeatTimer;
    QTimer     *m_subscribeHeartbeatTimer;
    bool        m_commandPingOutstanding;
    // more efficient to reuse a protobuf Message
    pb::Container   m_rx;
    pb::Container   m_tx;

    void start();
    void stop();
    void cleanup();
    void startCommandHeartbeat();
    void stopCommandHeartbeat();
    void startSubscribeHeartbeat(int interval);
    void stopSubscribeHeartbeat();
    void refreshSubscribeHeartbeat();
    void updateState(MachinetalkService::State state);
    void updateState(MachinetalkService::State state, MachinetalkService::ConnectionError error, QString errorString);
    void updateError(MachinetalkService::ConnectionError error, QString errorString);
    void sendCommandMessage(pb::ContainerType type);
    void updateSync();
    void clearSync();
    void initializeObject();

private slots:
    void subscribeMessageReceived(const QList<QByteArray> &messageList);
    void commandMessageReceived(const QList<QByteArray> &messageList);
    void pollError(int errorNum, const QString& errorMsg);
    void commandHeartbeatTimerTick();
    void subscribeHeartbeatTimerTick();

    bool connectSockets();
    void disconnectSockets();
    void subscribe(const QString &topic);
    void unsubscribe(const QString &topic);

signals:
    void launchercmdUriChanged(QString arg);
    void launcherUriChanged(QString arg);
    void heartbeatPeriodChanged(int arg);
    void connectedChanged(bool arg);
    void connectionStateChanged(MachinetalkService::State arg);
    void errorChanged(MachinetalkService::ConnectionError arg);
    void errorStringChanged(QString arg);
    void launchersChanged(QJsonValue arg);
    void syncedChanged(bool arg);
}; // class ApplicationLauncher
}; // namespace qtquickvcp

#endif // APPLICATIONLAUNCHER_H
