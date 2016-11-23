/****************************************************************************
**
** Copyright (C) 2014 Alexander Rössler
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

#include "applicationstatus.h"
#include "debughelper.h"

#if defined(Q_OS_IOS)
namespace gpb = google_public::protobuf;
#else
namespace gpb = google::protobuf;
#endif

using namespace nzmqt;

namespace qtquickvcp {

ApplicationStatus::ApplicationStatus(QObject *parent) :
    AbstractServiceImplementation(parent),
    m_statusUri(""),
    m_statusSocketState(Down),
    m_connected(false),
    m_connectionState(Disconnected),
    m_error(NoError),
    m_errorString(""),
    m_running(false),
    m_synced(false),
    m_channels(MotionChannel | ConfigChannel | IoChannel | TaskChannel | InterpChannel),
    m_context(nullptr),
    m_statusSocket(nullptr),
    m_statusHeartbeatTimer(new QTimer(this))
{
    connect(m_statusHeartbeatTimer, &QTimer::timeout,
            this, &ApplicationStatus::statusHeartbeatTimerTick);

    connect(this, &ApplicationStatus::taskChanged,
            this, &ApplicationStatus::updateRunning);
    connect(this, &ApplicationStatus::interpChanged,
            this, &ApplicationStatus::updateRunning);


    initializeObject(MotionChannel);
    initializeObject(ConfigChannel);
    initializeObject(IoChannel);
    initializeObject(TaskChannel);
    initializeObject(InterpChannel);
}

void ApplicationStatus::start()
{
#ifdef QT_DEBUG
   DEBUG_TAG(1, "status", "start")
#endif
    updateState(Connecting);

    if (connectSockets())
    {
        subscribe();
    }
}

void ApplicationStatus::stop()
{
#ifdef QT_DEBUG
    DEBUG_TAG(1, "status", "stop")
#endif

    cleanup();
    updateState(Disconnected);  // clears also the error
}

void ApplicationStatus::cleanup()
{
    if (m_connected)
    {
        unsubscribe();
    }
    disconnectSockets();
    m_subscriptions.clear();
}

void ApplicationStatus::startStatusHeartbeat(int interval)
{
    m_statusHeartbeatTimer->stop();

    if (interval > 0)
    {
        m_statusHeartbeatTimer->setInterval(interval);
        m_statusHeartbeatTimer->start();
    }
}

void ApplicationStatus::stopStatusHeartbeat()
{
    m_statusHeartbeatTimer->stop();
}

void ApplicationStatus::refreshStatusHeartbeat()
{
    if (m_statusHeartbeatTimer->isActive())
    {
        m_statusHeartbeatTimer->stop();
        m_statusHeartbeatTimer->start();
    }
}

void ApplicationStatus::updateState(ApplicationStatus::State state)
{
    updateState(state, NoError, "");
}

void ApplicationStatus::updateState(ApplicationStatus::State state, ApplicationStatus::ConnectionError error, const QString &errorString)
{
    if (state != m_connectionState)
    {
        if (m_connected) // we are not connected anymore
        {
            stopStatusHeartbeat();
            clearSync();
            m_connected = false;
            emit connectedChanged(false);
        }
        else if (state == Connected) {
            m_connected = true;
            emit connectedChanged(true);
        }

        m_connectionState = state;
        emit connectionStateChanged(m_connectionState);

        if ((state == Disconnected) || (state == Error)) {
            initializeObject(MotionChannel);
            initializeObject(ConfigChannel);
            initializeObject(IoChannel);
            initializeObject(TaskChannel);
            initializeObject(InterpChannel);
        }
    }

    updateError(error, errorString);
}

void ApplicationStatus::updateError(ApplicationStatus::ConnectionError error, const QString &errorString)
{
    if (m_errorString != errorString)
    {
        m_errorString = errorString;
        emit errorStringChanged(m_errorString);
    }

    if (m_error != error)
    {
        if (error != NoError)
        {
            cleanup();
        }

        m_error = error;
        emit errorChanged(m_error);
    }
}

void ApplicationStatus::updateSync(ApplicationStatus::StatusChannel channel)
{
    m_syncedChannels |= channel;

    if (m_syncedChannels == m_channels) {
        m_synced = true;
        emit syncedChanged(m_synced);
    }
}

void ApplicationStatus::clearSync()
{
    m_synced = false;
    m_syncedChannels = 0;
    emit syncedChanged(m_synced);
}

void ApplicationStatus::updateMotion(const pb::EmcStatusMotion &motion)
{
    MachinetalkService::recurseMessage(motion, &m_motion);
    emit motionChanged(m_motion);
}

void ApplicationStatus::updateConfig(const pb::EmcStatusConfig &config)
{
    MachinetalkService::recurseMessage(config, &m_config);
    emit configChanged(m_config);
}

void ApplicationStatus::updateIo(const pb::EmcStatusIo &io)
{
    MachinetalkService::recurseMessage(io, &m_io);
    emit ioChanged(m_io);
}

void ApplicationStatus::updateTask(const pb::EmcStatusTask &task)
{
    MachinetalkService::recurseMessage(task, &m_task);
    emit taskChanged(m_task);
}

void ApplicationStatus::updateInterp(const pb::EmcStatusInterp &interp)
{
    MachinetalkService::recurseMessage(interp, &m_interp);
    emit interpChanged(m_interp);
}

void ApplicationStatus::statusMessageReceived(const QList<QByteArray> &messageList)
{
    QByteArray topic;

    topic = messageList.at(0);
    m_rx.ParseFromArray(messageList.at(1).data(), messageList.at(1).size());

#ifdef QT_DEBUG
    std::string s;
    gpb::TextFormat::PrintToString(m_rx, &s);
    DEBUG_TAG(3, "status", "update" << topic << QString::fromStdString(s))
#endif

    if ((m_rx.type() == pb::MT_EMCSTAT_FULL_UPDATE)
        || (m_rx.type() == pb::MT_EMCSTAT_INCREMENTAL_UPDATE))
    {
        if ((topic == "motion") && m_rx.has_emc_status_motion()) {
            updateMotion(m_rx.emc_status_motion());
            if (m_rx.type() == pb::MT_EMCSTAT_FULL_UPDATE) {
                updateSync(MotionChannel);
            }
        }

        if ((topic == "config") && m_rx.has_emc_status_config()) {
            updateConfig(m_rx.emc_status_config());
            if (m_rx.type() == pb::MT_EMCSTAT_FULL_UPDATE) {
                updateSync(ConfigChannel);
            }
        }

        if ((topic == "io") && m_rx.has_emc_status_io()) {
            updateIo(m_rx.emc_status_io());
            if (m_rx.type() == pb::MT_EMCSTAT_FULL_UPDATE) {
                updateSync(IoChannel);
            }
        }

        if ((topic == "task") && m_rx.has_emc_status_task()) {
            updateTask(m_rx.emc_status_task());
            if (m_rx.type() == pb::MT_EMCSTAT_FULL_UPDATE) {
                updateSync(TaskChannel);
            }
        }

        if ((topic == "interp") && m_rx.has_emc_status_interp()) {
            updateInterp(m_rx.emc_status_interp());
            if (m_rx.type() == pb::MT_EMCSTAT_FULL_UPDATE) {
                updateSync(InterpChannel);
            }
        }

        if (m_rx.type() == pb::MT_EMCSTAT_FULL_UPDATE)
        {
            if (m_statusSocketState != Up)
            {
                m_statusSocketState = Up;
                updateState(Connected);
            }

            if (m_rx.has_pparams())
            {
                pb::ProtocolParameters pparams = m_rx.pparams();
                startStatusHeartbeat(pparams.keepalive_timer() * 2); // wait double the time of the hearbeat interval
            }
        }
        else
        {
            refreshStatusHeartbeat();
        }

        return;
    }
    else if (m_rx.type() == pb::MT_PING)
    {
        if (m_statusSocketState == Up)
        {
            refreshStatusHeartbeat();
        }
        else
        {
            updateState(Connecting);
            unsubscribe();  // clean up previous subscription
            subscribe();    // trigger a fresh subscribe
        }

        return;
    }

#ifdef QT_DEBUG
    gpb::TextFormat::PrintToString(m_rx, &s);
    DEBUG_TAG(1, "status", "update: unknown message type: " << QString::fromStdString(s))
#endif
}

void ApplicationStatus::pollError(int errorNum, const QString &errorMsg)
{
    QString errorString;
    errorString = QString("Error %1: ").arg(errorNum) + errorMsg;
    updateState(Error, SocketError, errorString);
}

void ApplicationStatus::statusHeartbeatTimerTick()
{
    m_statusSocketState = Down;
    updateState(Timeout);

#ifdef QT_DEBUG
    DEBUG_TAG(1, "status", "timeout")
#endif
}

/** Connects the 0MQ sockets */
bool ApplicationStatus::connectSockets()
{
    m_context = new PollingZMQContext(this, 1);
    connect(m_context, &PollingZMQContext::pollError,
            this, &ApplicationStatus::pollError);
    m_context->start();

    m_statusSocket = m_context->createSocket(ZMQSocket::TYP_SUB, this);
    m_statusSocket->setLinger(0);

    try {
        m_statusSocket->connectTo(m_statusUri);
    }
    catch (const zmq::error_t &e) {
        QString errorString;
        errorString = QString("Error %1: ").arg(e.num()) + QString(e.what());
        updateState(Error, SocketError, errorString);
        return false;
    }

    connect(m_statusSocket, &ZMQSocket::messageReceived,
            this, &ApplicationStatus::statusMessageReceived);

#ifdef QT_DEBUG
    DEBUG_TAG(1, "status", "socket connected" << m_statusUri)
#endif

    return true;
}

/** Disconnects the 0MQ sockets */
void ApplicationStatus::disconnectSockets()
{
    m_statusSocketState = Down;

    if (m_statusSocket != nullptr)
    {
        m_statusSocket->close();
        m_statusSocket->deleteLater();
        m_statusSocket = nullptr;
    }

    if (m_context != nullptr)
    {
        m_context->stop();
        m_context->deleteLater();
        m_context = nullptr;
    }
}

void ApplicationStatus::subscribe()
{
    m_statusSocketState = Trying;

    if (m_channels | MotionChannel) {
        m_statusSocket->subscribeTo("motion");
        m_subscriptions.append("motion");
    }
    if (m_channels | ConfigChannel) {
        m_statusSocket->subscribeTo("config");
        m_subscriptions.append("config");
    }
    if (m_channels | TaskChannel) {
        m_statusSocket->subscribeTo("task");
        m_subscriptions.append("task");
    }
    if (m_channels | IoChannel) {
        m_statusSocket->subscribeTo("io");
        m_subscriptions.append("io");
    }
    if (m_channels | InterpChannel) {
        m_statusSocket->subscribeTo("interp");
        m_subscriptions.append("interp");
    }
}

void ApplicationStatus::unsubscribe()
{
    m_statusSocketState = Down;

    foreach (QString subscription, m_subscriptions)
    {
        m_statusSocket->unsubscribeFrom(subscription);

        if (subscription == "motion") {
            initializeObject(MotionChannel);
        }
        else if (subscription == "config") {
            initializeObject(ConfigChannel);
        }
        else if (subscription == "io") {
            initializeObject(IoChannel);
        }
        else if (subscription == "task") {
            initializeObject(TaskChannel);
        }
        else if (subscription == "interp") {
            initializeObject(InterpChannel);
        }
    }
    m_subscriptions.clear();
}

void ApplicationStatus::updateRunning(const QJsonObject &object)
{
    Q_UNUSED(object)

    bool running;

    running = m_task["taskMode"].isDouble() && m_interp["interpState"].isDouble()
            && ((static_cast<int>(m_task["taskMode"].toDouble()) == TaskModeAuto)
                || (static_cast<int>(m_task["taskMode"].toDouble()) == TaskModeMdi))
            && (static_cast<int>(m_interp["interpState"].toDouble()) != InterpreterIdle);

    if (running != m_running)
    {
        m_running = running;
        emit runningChanged(running);
    }
}

void ApplicationStatus::initializeObject(ApplicationStatus::StatusChannel channel)
{
    switch (channel)
    {
    case MotionChannel:
        m_motion = QJsonObject();
        MachinetalkService::recurseDescriptor(pb::EmcStatusMotion::descriptor(), &m_motion);
        emit motionChanged(m_motion);
        return;
    case ConfigChannel:
        m_config = QJsonObject();
        MachinetalkService::recurseDescriptor(pb::EmcStatusConfig::descriptor(), &m_config);
        emit configChanged(m_config);
        return;
    case IoChannel:
        m_io = QJsonObject();
        MachinetalkService::recurseDescriptor(pb::EmcStatusIo::descriptor(), &m_io);
        emit ioChanged(m_io);
        return;
    case TaskChannel:
        m_task = QJsonObject();
        MachinetalkService::recurseDescriptor(pb::EmcStatusTask::descriptor(), &m_task);
        emit taskChanged(m_task);
        return;
    case InterpChannel:
        m_interp = QJsonObject();
        MachinetalkService::recurseDescriptor(pb::EmcStatusInterp::descriptor(), &m_interp);
        emit interpChanged(m_interp);
        return;
    }
}
}; // namespace qtquickvcp
