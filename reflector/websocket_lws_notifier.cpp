#ifdef USE_LIBWEBSOCKETS

#include "websocket_lws_notifier.h"
#include "Global.h"
#include "Notifier.h"
#include <algorithm>
#include <ctime>
#include <cstring>
#include <iostream>
#include <vector>
#include <nlohmann/json.hpp>

// Helper to get instance pointer from context user
static CWebsocketLwsNotifier *GetInstanceFromContext(struct lws_context *ctx)
{
    return reinterpret_cast<CWebsocketLwsNotifier*>(lws_context_user(ctx));
}

// Protocol callback
static int lws_protocol_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    struct lws_context *ctx = lws_get_context(wsi);
    CWebsocketLwsNotifier *inst = GetInstanceFromContext(ctx);
    if (!inst) return 0;
    return CWebsocketLwsNotifier::Callback(wsi, reason, user, in, len);
}

CWebsocketLwsNotifier::CWebsocketLwsNotifier()
    : m_Ctx(nullptr), m_Running(false), m_MaxClients(64), m_PerClientQueueLen(32),
      m_PingInterval(30), m_IdleTimeout(90), m_Port(0)
{
    // prepare protocols array
    memset(m_Protocols, 0, sizeof(m_Protocols));
    m_Protocols[0].name = "urfd-protocol";
    m_Protocols[0].callback = lws_protocol_callback;
    m_Protocols[0].per_session_data_size = sizeof(PerConn);
    m_Protocols[0].rx_buffer_size = 0;
    m_Protocols[0].id = 0;
    m_Protocols[1].name = nullptr; // terminator
}

CWebsocketLwsNotifier::~CWebsocketLwsNotifier()
{
    Stop();
}

bool CWebsocketLwsNotifier::Start(const std::string &bindAddr, uint16_t port, int maxClients, int perClientQueue,
    int pingIntervalSec, int idleTimeoutSec)
{
    if (m_Running) return false;
    m_Bind = bindAddr;
    m_Port = port;
    m_MaxClients = maxClients;
    m_PerClientQueueLen = perClientQueue;
    m_PingInterval = std::chrono::seconds(std::max(5, pingIntervalSec));
    m_IdleTimeout = std::chrono::seconds(std::max<int>(static_cast<int>(m_PingInterval.count()) * 3, idleTimeoutSec));

    // libwebsockets emits its own verbose startup chatter to stderr. Keep it quiet
    // in normal builds so it doesn't pollute the rest of the reflector logs; allow
    // higher levels only in debug builds.
#ifdef DEBUG
    lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);
#else
    lws_set_log_level(0, nullptr);
#endif

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port;
    info.iface = m_Bind.empty() ? nullptr : m_Bind.c_str();
    info.protocols = m_Protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
    // store 'this' as user ptr on context so callback can retrieve it
    info.user = this;

    m_Ctx = lws_create_context(&info);
    if (!m_Ctx)
    {
        std::cerr << "websocket_lws_notifier: lws_create_context failed" << std::endl;
        return false;
    }

    m_Running = true;
    m_Thread = std::thread(&CWebsocketLwsNotifier::ServiceLoop, this);
    return true;
}

void CWebsocketLwsNotifier::Stop()
{
    if (!m_Running) return;
    m_Running = false;
    if (m_Ctx)
    {
        lws_cancel_service(m_Ctx);
    }
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        for (auto &kv : m_Clients)
        {
            if (kv.first)
            {
                unsigned char closeBuf[] = "shutdown";
                lws_close_reason(kv.first, LWS_CLOSE_STATUS_GOINGAWAY,
                    closeBuf, sizeof(closeBuf) - 1);
            }
        }
        m_Clients.clear();
    }
    if (m_Thread.joinable()) m_Thread.join();
    if (m_Ctx)
    {
        lws_context_destroy(m_Ctx);
        m_Ctx = nullptr;
    }
}

void CWebsocketLwsNotifier::Publish(const std::string &msg)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    // append to global queue
    m_GlobalQueue.push_back(msg);
    // move messages into per-connection queues, but do not exceed per-client queue len
    for (auto &kv : m_Clients)
    {
        auto &cq = kv.second.queue;
        if ((int)cq.size() >= m_PerClientQueueLen)
        {
            // drop client by closing its wsi
            struct lws *wsi = kv.first;
            std::cerr << "websocket_lws_notifier: closing slow client due to full queue" << std::endl;
            lws_set_timeout(wsi, PENDING_TIMEOUT_USER_OK, LWS_TO_KILL_ASYNC);
            continue;
        }
        cq.push_back(msg);
        // notify lws that this client wants to write
        lws_callback_on_writable(kv.first);
    }
    // wake up service loop
    if (m_Ctx) lws_cancel_service(m_Ctx);
}

int CWebsocketLwsNotifier::Callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
    CWebsocketLwsNotifier *inst = GetInstanceFromContext(lws_get_context(wsi));
    if (!inst) return 0;
    switch (reason)
    {
        case LWS_CALLBACK_ESTABLISHED:
            inst->OnEstablished(wsi);
            break;
        case LWS_CALLBACK_RECEIVE:
            inst->OnReceive(wsi, in, len);
            break;
        case LWS_CALLBACK_RECEIVE_PONG:
            inst->OnReceive(wsi, in, len);
            break;
        case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
            inst->OnClosed(wsi);
            break;
        case LWS_CALLBACK_CLOSED:
            inst->OnClosed(wsi);
            break;
        case LWS_CALLBACK_SERVER_WRITEABLE:
            inst->OnWritable(wsi);
            break;
        case LWS_CALLBACK_TIMER:
            inst->OnTimer(wsi);
            break;
        default:
            break;
    }
    return 0;
}

static nlohmann::json BuildReflectorSnapshot()
{
    const auto now = std::time(nullptr);
    const auto started = g_Reflector.GetStartedAt();

    nlohmann::json snapshot = nlohmann::json::object();
    snapshot["server_started"] = started;
    snapshot["current_time"] = now;
    snapshot["uptime_seconds"] = static_cast<std::int64_t>(now - started);
    snapshot["links"] = nlohmann::json::array();
    snapshot["last_heard"] = nlohmann::json::array();
    snapshot["open_channels"] = nlohmann::json::array();

    auto *clients = g_Reflector.GetClients();
    if (clients)
    {
        for (const auto &client : *clients)
        {
            nlohmann::json entry;
            entry["callsign"] = client->GetCallsign().GetCS();
            entry["on_module"] = std::string(1, client->GetReflectorModule());
            entry["protocol"] = client->GetProtocolName();
            if (client->GetConnectTime() > 0)
                entry["connect_time"] = client->GetConnectTime();
            snapshot["links"].push_back(entry);
        }
        g_Reflector.ReleaseClients();
    }

    auto *users = g_Reflector.GetUsers();
    if (users)
    {
        for (const auto &user : *users)
        {
            nlohmann::json entry;
            entry["callsign"] = user.GetCallsign();
            entry["on_module"] = std::string(1, user.GetOnModule());
            entry["via_peer"] = user.GetViaPeer();
            entry["via_node"] = user.GetViaNode();
            entry["protocol"] = user.GetProtocol();
            entry["last_heard_time"] = user.GetLastHeardTime();
            if (!user.GetLocator().empty())
                entry["locator"] = user.GetLocator();
            snapshot["last_heard"].push_back(entry);
        }
        g_Reflector.ReleaseUsers();
    }

    const std::string modules = g_Reflector.GetModules();
    for (char module : modules)
    {
        auto stream = g_Reflector.GetStream(module);
        if (!stream || !stream->IsOpen())
            continue;

        nlohmann::json entry;
        entry["module"] = std::string(1, module);
        entry["stream_id"] = stream->GetStreamId();
        entry["user"] = stream->GetUserCallsign().GetCS();

        auto owner = stream->GetOwnerClient();
        if (owner)
        {
            entry["callsign"] = owner->GetCallsign().GetCS();
            entry["protocol"] = owner->GetProtocolName();
            entry["linked_module"] = std::string(1, owner->GetReflectorModule());
        }
        snapshot["open_channels"].push_back(entry);
    }

    return snapshot;
}

void CWebsocketLwsNotifier::OnEstablished(struct lws *wsi)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    if ((int)m_Clients.size() >= m_MaxClients)
    {
        std::cerr << "websocket_lws_notifier: max clients reached, closing connection" << std::endl;
        unsigned char closeBuf[] = "full";
        lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION,
            closeBuf, sizeof(closeBuf) - 1);
        return;
    }
    auto result = m_Clients.emplace(wsi, PerConn());
    if (!result.second)
    {
        unsigned char closeBuf[] = "dup";
        lws_close_reason(wsi, LWS_CLOSE_STATUS_PROTOCOL_ERR,
            closeBuf, sizeof(closeBuf) - 1);
        return;
    }
    result.first->second.lastSeen = std::chrono::steady_clock::now();
    lws_set_timer_usecs(wsi, std::chrono::duration_cast<std::chrono::microseconds>(m_PingInterval).count());

    try
    {
        CEvent ev;
        ev.event_type = "state_snapshot";
        ev.timestamp = static_cast<std::int64_t>(std::time(nullptr));
        if (!g_Configure.GetString(g_Keys.names.callsign).empty())
            ev.reflector_name = g_Configure.GetString(g_Keys.names.callsign);
        ev.payload = BuildReflectorSnapshot().dump();

        const std::string msg = ev.ToJson().dump();
        result.first->second.queue.push_back(msg);
        lws_callback_on_writable(wsi);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "websocket_lws_notifier: failed to build state snapshot: " << ex.what() << std::endl;
    }

    std::cout << "websocket_lws_notifier: client connected" << std::endl;
}

void CWebsocketLwsNotifier::OnClosed(struct lws *wsi)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Clients.find(wsi);
    if (it != m_Clients.end()) m_Clients.erase(it);
    std::cout << "websocket_lws_notifier: client disconnected" << std::endl;
}

void CWebsocketLwsNotifier::OnReceive(struct lws *wsi, void *in, size_t len)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Clients.find(wsi);
    if (it == m_Clients.end()) return;
    it->second.lastSeen = std::chrono::steady_clock::now();
    it->second.lastPingSent = std::chrono::steady_clock::time_point::min();
    lws_set_timer_usecs(wsi, std::chrono::duration_cast<std::chrono::microseconds>(m_PingInterval).count());
    (void)in;
    (void)len;
}

void CWebsocketLwsNotifier::OnWritable(struct lws *wsi)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Clients.find(wsi);
    if (it == m_Clients.end()) return;
    auto &q = it->second.queue;
    if (q.empty()) return;
    const std::string msg = q.front();
    q.pop_front();

    // prepare buffer with LWS_PRE
    std::vector<unsigned char> buf(LWS_PRE + msg.size());
    memcpy(buf.data() + LWS_PRE, msg.data(), msg.size());
    int n = lws_write(wsi, buf.data() + LWS_PRE, msg.size(), LWS_WRITE_TEXT);
    if (n < 0)
    {
        std::cerr << "websocket_lws_notifier: lws_write failed" << std::endl;
        unsigned char closeBuf[] = "write";
        lws_close_reason(wsi, LWS_CLOSE_STATUS_PROTOCOL_ERR,
            closeBuf, sizeof(closeBuf) - 1);
    }
    else
    {
        it->second.lastSeen = std::chrono::steady_clock::now();
        if (!q.empty()) lws_callback_on_writable(wsi);
    }
}

void CWebsocketLwsNotifier::OnTimer(struct lws *wsi)
{
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_Clients.find(wsi);
    if (it == m_Clients.end()) return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastSeen);
    if (elapsed >= m_IdleTimeout)
    {
        std::cerr << "websocket_lws_notifier: idle client exceeded timeout; closing connection" << std::endl;
        unsigned char closeBuf[] = "idle";
        lws_close_reason(wsi, LWS_CLOSE_STATUS_GOINGAWAY,
            closeBuf, sizeof(closeBuf) - 1);
        return;
    }

    if (elapsed >= m_PingInterval)
    {
        const std::string pingPayload = "urfd";
        if (it->second.lastPingSent == std::chrono::steady_clock::time_point::min() ||
            now - it->second.lastPingSent >= m_PingInterval)
        {
            std::vector<unsigned char> pingBuf(LWS_PRE + pingPayload.size());
            memcpy(pingBuf.data() + LWS_PRE, pingPayload.data(), pingPayload.size());
            int rc = lws_write(wsi, pingBuf.data() + LWS_PRE, pingPayload.size(), LWS_WRITE_PING);
            if (rc >= 0)
            {
                it->second.lastPingSent = now;
            }
            else
            {
                std::cerr << "websocket_lws_notifier: ping failed; closing client" << std::endl;
                unsigned char closeBuf[] = "ping";
                lws_close_reason(wsi, LWS_CLOSE_STATUS_PROTOCOL_ERR,
                    closeBuf, sizeof(closeBuf) - 1);
                return;
            }
        }
    }

    lws_set_timer_usecs(wsi, std::chrono::duration_cast<std::chrono::microseconds>(m_PingInterval).count());
}

void CWebsocketLwsNotifier::ServiceLoop()
{
    while (m_Running && m_Ctx)
    {
        // service for a short time
        lws_service(m_Ctx, 100);
    }
}

#endif // USE_LIBWEBSOCKETS
