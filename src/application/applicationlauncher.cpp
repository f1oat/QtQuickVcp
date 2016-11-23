#include "applicationlauncher.h"
#include "debughelper.h"

#if defined(Q_OS_IOS)
namespace gpb = google_public::protobuf;
#else
namespace gpb = google::protobuf;
#endif

using namespace nzmqt;

namespace qtquickvcp {

ApplicationLauncher::ApplicationLauncher(QObject *parent) :
    AbstractServiceImplementation(parent),
    m_subscribeUri(""),
    m_commandUri(""),
    m_commandIdentity("launcher"),
    m_heartbeatPeriod(3000),
    m_connected(false),
    m_subscribeSocketState(MachinetalkService::Down),
    m_commandSocketState(MachinetalkService::Down),
    m_connectionState(MachinetalkService::Disconnected),
    m_error(MachinetalkService::NoError),
    m_errorString(""),
    m_launchers(QJsonValue(QJsonArray())),
    m_synced(false),
    m_context(nullptr),
    m_subscribeSocket(nullptr),
    m_commandSocket(nullptr),
    m_commandHeartbeatTimer(new QTimer(this)),
    m_subscribeHeartbeatTimer(new QTimer(this)),
    m_commandPingOutstanding(false)
{
    connect(m_commandHeartbeatTimer, &QTimer::timeout,
            this, &ApplicationLauncher::commandHeartbeatTimerTick);
    connect(m_subscribeHeartbeatTimer, &QTimer::timeout,
            this, &ApplicationLauncher::subscribeHeartbeatTimerTick);

    initializeObject();
}

ApplicationLauncher::~ApplicationLauncher()
{
    MachinetalkService::removeTempPath("launcher"); // clean up dir created by json
}

void ApplicationLauncher::start(int index)
{
    if (!m_connected) {
        return;
    }

#ifdef QT_DEBUG
    DEBUG_TAG(1, m_commandIdentity, "starting launcher" << index)
#endif

    m_tx.set_index(index);
    sendCommandMessage(pb::MT_LAUNCHER_START);
}

void ApplicationLauncher::kill(int index)
{
    if (!m_connected) {
        return;
    }

    m_tx.set_index(index);
    sendCommandMessage(pb::MT_LAUNCHER_KILL);
}

void ApplicationLauncher::terminate(int index)
{
    if (!m_connected) {
        return;
    }

    m_tx.set_index(index);
    sendCommandMessage(pb::MT_LAUNCHER_TERMINATE);
}

void ApplicationLauncher::writeToStdin(int index, const QString &data)
{
    if (!m_connected) {
        return;
    }

    m_tx.set_index(index);
    m_tx.set_name(data.toStdString());
    sendCommandMessage(pb::MT_LAUNCHER_WRITE_STDIN);
}

void ApplicationLauncher::call(const QString &command)
{
    if (!m_connected) {
        return;
    }

    m_tx.set_name(command.toStdString());
    sendCommandMessage(pb::MT_LAUNCHER_CALL);
}

void ApplicationLauncher::shutdown()
{
    if (!m_connected) {
        return;
    }

    sendCommandMessage(pb::MT_LAUNCHER_SHUTDOWN);
}

/** Connects the 0MQ sockets */
bool ApplicationLauncher::connectSockets()
{
    m_context = new PollingZMQContext(this, 1);
    connect(m_context, &PollingZMQContext::pollError,
            this, &ApplicationLauncher::pollError);
    m_context->start();

    m_commandSocket = m_context->createSocket(ZMQSocket::TYP_DEALER, this);
    m_commandSocket->setLinger(0);
    m_commandSocket->setIdentity(QString("%1-%2").arg(m_commandIdentity).arg(QCoreApplication::applicationPid()).toLocal8Bit());

    m_subscribeSocket = m_context->createSocket(ZMQSocket::TYP_SUB, this);
    m_subscribeSocket->setLinger(0);

    try {
        m_commandSocket->connectTo(m_commandUri);
        m_subscribeSocket->connectTo(m_subscribeUri);
    }
    catch (const zmq::error_t &e) {
        QString errorString;
        errorString = QString("Error %1: ").arg(e.num()) + QString(e.what());
        updateState(MachinetalkService::Error, MachinetalkService::SocketError, errorString);
        return false;
    }

    connect(m_subscribeSocket, &ZMQSocket::messageReceived,
            this, &ApplicationLauncher::subscribeMessageReceived);
    connect(m_commandSocket, &ZMQSocket::messageReceived,
            this, &ApplicationLauncher::commandMessageReceived);

#ifdef QT_DEBUG
    DEBUG_TAG(1, m_commandIdentity, "sockets connected" << m_subscribeUri << m_commandUri)
#endif

    return true;
}

/** Disconnects the 0MQ sockets */
void ApplicationLauncher::disconnectSockets()
{
    m_commandSocketState = MachinetalkService::Down;
    m_subscribeSocketState = MachinetalkService::Down;

    if (m_commandSocket != nullptr)
    {
        m_commandSocket->close();
        m_commandSocket->deleteLater();
        m_commandSocket = nullptr;
    }

    if (m_subscribeSocket != nullptr)
    {
        m_subscribeSocket->close();
        m_subscribeSocket->deleteLater();
        m_subscribeSocket = nullptr;
    }

    if (m_context != nullptr)
    {
        m_context->stop();
        m_context->deleteLater();
        m_context = nullptr;
    }
}

void ApplicationLauncher::subscribe(const QString &topic)
{
    m_subscribeSocketState = MachinetalkService::Trying;
    m_subscribeSocket->subscribeTo(topic.toLocal8Bit());
}

void ApplicationLauncher::unsubscribe(const QString &topic)
{
    m_subscribeSocketState = MachinetalkService::Down;
    m_subscribeSocket->unsubscribeFrom(topic.toLocal8Bit());
}

void ApplicationLauncher::start()
{
#ifdef QT_DEBUG
   DEBUG_TAG(1, m_commandIdentity, "start")
#endif
    m_commandSocketState = MachinetalkService::Trying;
    updateState(MachinetalkService::Connecting);

    if (connectSockets())
    {
        subscribe("launcher");
        startCommandHeartbeat();
        sendCommandMessage(pb::MT_PING);
    }
}

void ApplicationLauncher::stop()
{
#ifdef QT_DEBUG
    DEBUG_TAG(1, m_commandIdentity, "stop")
#endif

    cleanup();

    updateState(MachinetalkService::Disconnected);  // clears also the error
}

void ApplicationLauncher::cleanup()
{
    if (m_connected)
    {
        unsubscribe("launcher");
    }
    stopCommandHeartbeat();
    disconnectSockets();
}

void ApplicationLauncher::startCommandHeartbeat()
{
    m_commandPingOutstanding = false;

    if (m_heartbeatPeriod > 0)
    {
        m_commandHeartbeatTimer->setInterval(m_heartbeatPeriod);
        m_commandHeartbeatTimer->start();
    }
}

void ApplicationLauncher::stopCommandHeartbeat()
{
    m_commandHeartbeatTimer->stop();
}

void ApplicationLauncher::startSubscribeHeartbeat(int interval)
{
    m_subscribeHeartbeatTimer->stop();

    if (interval > 0)
    {
        m_subscribeHeartbeatTimer->setInterval(interval);
        m_subscribeHeartbeatTimer->start();
    }
}

void ApplicationLauncher::stopSubscribeHeartbeat()
{
    m_subscribeHeartbeatTimer->stop();
}

void ApplicationLauncher::refreshSubscribeHeartbeat()
{
    if (m_subscribeHeartbeatTimer->isActive())
    {
        m_subscribeHeartbeatTimer->stop();
        m_subscribeHeartbeatTimer->start();
    }
}

void ApplicationLauncher::updateState(MachinetalkService::State state)
{
    updateState(state, MachinetalkService::NoError, "");
}

void ApplicationLauncher::updateState(MachinetalkService::State state, MachinetalkService::ConnectionError error, QString errorString)
{
    if (state != m_connectionState)
    {
        if (m_connected) // we are not connected anymore
        {
            stopSubscribeHeartbeat();
            clearSync();
            m_connected = false;
            emit connectedChanged(false);
        }
        else if (state == MachinetalkService::Connected) {
            m_connected = true;
            emit connectedChanged(true);
        }

        m_connectionState = state;
        emit connectionStateChanged(m_connectionState);

        if ((state == MachinetalkService::Disconnected) || (state == MachinetalkService::Error)) {
            initializeObject();
        }
    }

    updateError(error, errorString);
}

void ApplicationLauncher::updateError(MachinetalkService::ConnectionError error, QString errorString)
{
    if (m_errorString != errorString)
    {
        m_errorString = errorString;
        emit errorStringChanged(m_errorString);
    }

    if (m_error != error)
    {
        if (error != MachinetalkService::NoError)
        {
            cleanup();
        }
        m_error = error;
        emit errorChanged(m_error);
    }
}

void ApplicationLauncher::pollError(int errorNum, const QString &errorMsg)
{
    QString errorString;
    errorString = QString("Error %1: ").arg(errorNum) + errorMsg;
    updateState(MachinetalkService::Error, MachinetalkService::SocketError, errorString);
}

/** Processes all message received on the update 0MQ socket */
void ApplicationLauncher::subscribeMessageReceived(const QList<QByteArray> &messageList)
{
    QByteArray topic;

    topic = messageList.at(0);
    m_rx.ParseFromArray(messageList.at(1).data(), messageList.at(1).size());

#ifdef QT_DEBUG
    std::string s;
    gpb::TextFormat::PrintToString(m_rx, &s);
    DEBUG_TAG(3, m_commandIdentity, "launcher update" << topic << QString::fromStdString(s))
#endif

    if (m_rx.type() == pb::MT_LAUNCHER_FULL_UPDATE) //value update
    {
        m_launchers = QJsonValue(QJsonArray()); // clear old value
        MachinetalkService::updateValue(m_rx, &m_launchers, "launcher", "launcher"); // launcher protobuf value, launcher temp path
        emit launchersChanged(m_launchers);

        if (m_subscribeSocketState != MachinetalkService::Up)
        {
            m_subscribeSocketState = MachinetalkService::Up;
            updateState(MachinetalkService::Connected);
        }

        updateSync();

        if (m_rx.has_pparams())
        {
            pb::ProtocolParameters pparams = m_rx.pparams();
            startSubscribeHeartbeat(pparams.keepalive_timer() * 2);  // wait double the time of the hearbeat interval
        }
    }
    else if (m_rx.type() == pb::MT_LAUNCHER_INCREMENTAL_UPDATE){
        MachinetalkService::updateValue(m_rx, &m_launchers, "launcher", "launcher"); // launcher protobuf value, launcher temp path
        emit launchersChanged(m_launchers);

        refreshSubscribeHeartbeat();
    }
    else if (m_rx.type() == pb::MT_PING)
    {
        if (m_subscribeSocketState == MachinetalkService::Up)
        {
            refreshSubscribeHeartbeat();
        }
        else
        {
            updateState(MachinetalkService::Connecting);
            unsubscribe("launcher");  // clean up previous subscription
            subscribe("launcher");    // trigger a fresh subscribe -> full update
        }
    }
    else if (m_rx.type() == pb::MT_LAUNCHER_ERROR)
    {
        QString errorString;

        for (int i = 0; i < m_rx.note_size(); ++i)
        {
            errorString.append(QString::fromStdString(m_rx.note(i)) + "\n");
        }

        m_subscribeSocketState = MachinetalkService::Down;
        updateState(MachinetalkService::Error, MachinetalkService::CommandError, errorString);

#ifdef QT_DEBUG
        DEBUG_TAG(1, m_commandIdentity, "proto error on subscribe" << errorString)
#endif
    }
    else
    {
#ifdef QT_DEBUG
        gpb::TextFormat::PrintToString(m_rx, &s);
        DEBUG_TAG(1, m_commandIdentity, "status_update: unknown message type: " << QString::fromStdString(s))
#endif
    }
}

/** Processes all message received on the command 0MQ socket */
void ApplicationLauncher::commandMessageReceived(const QList<QByteArray> &messageList)
{
    m_rx.ParseFromArray(messageList.at(0).data(), messageList.at(0).size());

#ifdef QT_DEBUG
    std::string s;
    gpb::TextFormat::PrintToString(m_rx, &s);
    DEBUG_TAG(3, m_commandIdentity, "server message" << QString::fromStdString(s))
#endif

    if (m_rx.type() == pb::MT_PING_ACKNOWLEDGE)
    {
        m_commandPingOutstanding = false;

        if (m_commandSocketState != MachinetalkService::Up)
        {
            m_commandSocketState = MachinetalkService::Up;
            updateState(MachinetalkService::Connected);
        }

#ifdef QT_DEBUG
        DEBUG_TAG(2, m_commandIdentity, "ping ack")
#endif

        return;
    }
    else if (m_rx.type() == pb::MT_ERROR)
    {
        QString errorString;

        for (int i = 0; i < m_rx.note_size(); ++i)
        {
            errorString.append(QString::fromStdString(m_rx.note(i)) + "\n");
        }

        m_commandSocketState = MachinetalkService::Down;
        updateState(MachinetalkService::Error, MachinetalkService::ServiceError, errorString);

#ifdef QT_DEBUG
        DEBUG_TAG(1, "command", "error" << errorString)
#endif

        return;
    }
    else
    {
#ifdef QT_DEBUG
        DEBUG_TAG(1, m_commandIdentity, "UNKNOWN server message type")
#endif
    }
}

void ApplicationLauncher::sendCommandMessage(pb::ContainerType type)
{
    if (m_commandSocket == nullptr) {  // disallow sending messages when not connected
        return;
    }

    try {
        m_tx.set_type(type);
        m_commandSocket->sendMessage(QByteArray(m_tx.SerializeAsString().c_str(), m_tx.ByteSize()));
        m_tx.Clear();
    }
    catch (const zmq::error_t &e) {
        QString errorString;
        errorString = QString("Error %1: ").arg(e.num()) + QString(e.what());
        updateState(MachinetalkService::Error, MachinetalkService::SocketError, errorString);
    }
}

void ApplicationLauncher::updateSync()
{
    m_synced = true;
    emit syncedChanged(m_synced);
}

void ApplicationLauncher::clearSync()
{
    m_synced = false;
    emit syncedChanged(m_synced);
    initializeObject();
}

void ApplicationLauncher::initializeObject()
{
    m_launchers = QJsonValue(QJsonArray());
    emit launchersChanged(m_launchers);
}

void ApplicationLauncher::commandHeartbeatTimerTick()
{
    if (m_commandPingOutstanding)
    {
        m_commandSocketState = MachinetalkService::Trying;
        updateState(MachinetalkService::Timeout);

#ifdef QT_DEBUG
        DEBUG_TAG(1, m_commandIdentity, "launchercmd timeout")
#endif
    }

    sendCommandMessage(pb::MT_PING);

    m_commandPingOutstanding = true;

#ifdef QT_DEBUG
    DEBUG_TAG(2, m_commandIdentity, "ping")
#endif
}

void ApplicationLauncher::subscribeHeartbeatTimerTick()
{
    m_subscribeSocketState = MachinetalkService::Down;
    updateState(MachinetalkService::Timeout);

#ifdef QT_DEBUG
    DEBUG_TAG(1, m_commandIdentity, "launchercmd timeout")
#endif
}
}; // namespace qtquickvcp
