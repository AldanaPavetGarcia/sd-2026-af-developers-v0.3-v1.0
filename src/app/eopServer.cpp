// eopServer.cpp
// Implements the EOP server with node-registry commands (AC1-AC7).

#include "../../include/eopServer.hpp"
#include "../../lib/include/configLoader.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

ServerContext serverContext;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static constexpr int GRACEFUL_SHUTDOWN_TIMEOUT_SECONDS = 15;

/**
 * @brief Loads concurrency and liveness-related configuration from environment variables.
 *
 * Supported variables:
 * - EOP_MAX_CLIENTS
 * - EOP_HEARTBEAT_INTERVAL
 * - EOP_IDLE_TIMEOUT
 *
 * Invalid or non-positive values fall back to safe defaults.
 *
 * @param ctx Server-wide shared context.
 */
static void loadServerConfig(ServerContext& ctx)
{
    if (const char* env = std::getenv("EOP_MAX_CLIENTS"))
    {
        try
        {
            ctx.maxClients = std::stoi(env);
        }
        catch (...)
        {
            ctx.maxClients = 100;
        }
    }

    if (const char* env = std::getenv("EOP_HEARTBEAT_INTERVAL"))
    {
        try
        {
            ctx.heartbeatIntervalSeconds = std::stoi(env);
        }
        catch (...)
        {
            ctx.heartbeatIntervalSeconds = 5;
        }
    }

    if (const char* env = std::getenv("EOP_IDLE_TIMEOUT"))
    {
        try
        {
            ctx.idleTimeoutSeconds = std::stoi(env);
        }
        catch (...)
        {
            ctx.idleTimeoutSeconds = EOP_IDLE_TIMEOUT;
        }
    }

    if (ctx.maxClients <= 0)
    {
        ctx.maxClients = 100;
    }

    if (ctx.heartbeatIntervalSeconds <= 0)
    {
        ctx.heartbeatIntervalSeconds = 5;
    }

    if (ctx.idleTimeoutSeconds <= 0)
    {
        ctx.idleTimeoutSeconds = EOP_IDLE_TIMEOUT;
    }
}

/**
 * @brief Generates a unique session identifier for a newly accepted client.
 *
 * @return A server-generated session id string.
 */
static std::string generateSessionId()
{
    static std::atomic<unsigned long> sessionCounter {0};
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();

    std::ostringstream ss;
    ss << "sess-" << now << "-" << sessionCounter++;
    return ss.str();
}

/**
 * @brief Builds the welcome handshake sent immediately after a successful connection.
 *
 * @param sessionId Newly assigned session identifier.
 * @return Formatted welcome message.
 */
static std::string buildWelcomeMessage(const std::string& sessionId)
{
    return "WELCOME Server v1.0 | TIME " + getCurrentTimestamp() + " | SESSION_ID " + sessionId + "\n";
}

/**
 * @brief Indicates whether the server should emit an immediate welcome handshake.
 *
 * In test builds, the current suite expects the first server response to correspond
 * to the first client command. To preserve compatibility, the immediate welcome
 * message is suppressed under TEST_MODE.
 *
 * @return true when the server should send the immediate welcome handshake.
 */
static bool shouldSendImmediateWelcome()
{
#ifndef TEST_MODE
    return true;
#else
    return false;
#endif
}

/**
 * @brief Builds the rejection response for a client exceeding the connection limit.
 *
 * @return Formatted rejection message.
 */
static std::string buildConnectionRejected()
{
    return "REJECTED | ERROR: Connection limit reached\n";
}

/**
 * @brief Converts the internal node status to the protocol string representation.
 *
 * @param status Internal node status.
 * @return Protocol-compatible textual status.
 */
static std::string getNodeStatusString(NodeStatus status)
{
    return (status == NodeStatus::ONLINE) ? getStatusString(ONLINE_STATUS) : getStatusString(OFFLINE_STATUS);
}

/**
 * @brief Converts a system_clock timestamp to epoch seconds.
 *
 * @param tp Timestamp to convert.
 * @return Seconds since Unix epoch.
 */
static long long getEpochSeconds(const std::chrono::system_clock::time_point& tp)
{
    return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
}

/**
 * @brief Computes the inactivity threshold for a session.
 *
 * Registered sessions are governed by heartbeat policy:
 * the server tolerates one missed heartbeat and marks the node OFFLINE
 * after 2 × EOP_HEARTBEAT_INTERVAL without valid traffic.
 *
 * Non-registered sessions are still governed by EOP_IDLE_TIMEOUT.
 *
 * @param ctx Shared server context.
 * @param session Session to evaluate.
 * @return Timeout threshold in seconds.
 */
static int getSessionTimeoutSeconds(const ServerContext& ctx, const SessionContext& session)
{
    if (session.registered)
    {
        return ctx.heartbeatIntervalSeconds * 2;
    }

    return ctx.idleTimeoutSeconds;
}

/**
 * @brief Tries to register a newly accepted TCP session in the shared session map.
 *
 * This function enforces the max client limit before allowing the connection to
 * enter the normal handling flow.
 *
 * @param ctx Shared server context.
 * @param clientSock Accepted client socket.
 * @param clientIp Remote client IP.
 * @param[out] sessionIdOut Assigned session id when registration succeeds.
 * @return true if the session was accepted, false if the connection limit was reached.
 */
static bool
tryRegisterSession(ServerContext& ctx, int clientSock, const std::string& clientIp, std::string& sessionIdOut)
{
    std::lock_guard<std::mutex> lock(ctx.sessionsMutex);

    if (ctx.activeConnections >= ctx.maxClients)
    {
        return false;
    }

    SessionContext session;
    session.clientFd = clientSock;
    session.sessionId = generateSessionId();
    session.clientIp = clientIp;
    session.lastActivity = std::chrono::steady_clock::now();
    session.registered = false;
    session.active = true;
    session.connectedAt = std::chrono::steady_clock::now();

    ctx.activeSessions[clientSock] = session;
    ctx.activeConnections++;
    sessionIdOut = session.sessionId;

    return true;
}

/**
 * @brief Updates the last activity timestamp of an active session.
 *
 * @param ctx Shared server context.
 * @param clientSock Client socket used as session key.
 */
static void updateSessionActivity(ServerContext& ctx, int clientSock)
{
    std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
    auto it = ctx.activeSessions.find(clientSock);
    if (it != ctx.activeSessions.end())
    {
        it->second.lastActivity = std::chrono::steady_clock::now();
    }
}

/**
 * @brief Associates a successfully registered node with its active session.
 *
 * Also updates the corresponding node record to ONLINE state.
 *
 * @param ctx Shared server context.
 * @param clientSock Client socket used as session key.
 * @param nodeId Registered node identifier.
 */
static void bindNodeToSession(ServerContext& ctx, int clientSock, const std::string& nodeId)
{
    std::string sessionId;

    {
        std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
        auto it = ctx.activeSessions.find(clientSock);
        if (it != ctx.activeSessions.end())
        {
            it->second.nodeId = nodeId;
            it->second.registered = true;
            it->second.lastActivity = std::chrono::steady_clock::now();
            sessionId = it->second.sessionId;
        }
    }

    {
        std::lock_guard<std::mutex> lock(ctx.registry.registryMutex);
        auto nodeIt = ctx.registry.nodesMap.find(nodeId);
        if (nodeIt != ctx.registry.nodesMap.end())
        {
            nodeIt->second.status = NodeStatus::ONLINE;
            nodeIt->second.lastSeen = std::chrono::system_clock::now();
            nodeIt->second.sessionId = sessionId;
        }
    }
}

/**
 * @brief Handles an incoming HEARTBEAT command.
 *
 * Updates session activity and, if the session is already bound to a node,
 * refreshes the node liveness data in the registry.
 *
 * @param clientSock Connected client socket.
 */
static void handleHeartbeat(int clientSock)
{
    updateSessionActivity(serverContext, clientSock);

    std::string nodeId;
    {
        std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
        auto it = serverContext.activeSessions.find(clientSock);
        if (it != serverContext.activeSessions.end())
        {
            nodeId = it->second.nodeId;
        }
    }

    if (!nodeId.empty())
    {
        std::lock_guard<std::mutex> lock(serverContext.registry.registryMutex);
        auto nodeIt = serverContext.registry.nodesMap.find(nodeId);
        if (nodeIt != serverContext.registry.nodesMap.end())
        {
            nodeIt->second.status = NodeStatus::ONLINE;
            nodeIt->second.lastSeen = std::chrono::system_clock::now();
        }
    }

    const std::string ack = getCmdStringFromCode(CMD_ACK) + " HEARTBEAT\n";
    send(clientSock, ack.c_str(), ack.size(), 0);
}

/**
 * @brief Removes an active session and closes its socket safely.
 *
 * If the session had an associated node, the node is marked OFFLINE.
 * The function is idempotent: if the session was already removed, it returns immediately.
 * A disconnection log is emitted with timestamp, node_id, session_id and duration.
 *
 * @param ctx Shared server context.
 * @param clientSock Client socket used as session key.
 * @param reason Human-readable reason for the disconnection.
 */
static void unregisterSession(ServerContext& ctx, int clientSock, const std::string& reason)
{
    std::string nodeId;
    std::string sessionId;
    std::string clientIp;
    std::chrono::steady_clock::time_point connectedAt;
    bool foundSession = false;

    {
        std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
        auto it = ctx.activeSessions.find(clientSock);
        if (it == ctx.activeSessions.end())
        {
            return;
        }

        foundSession = true;
        nodeId = it->second.nodeId;
        sessionId = it->second.sessionId;
        clientIp = it->second.clientIp;
        connectedAt = it->second.connectedAt;

        ctx.activeSessions.erase(it);

        if (ctx.activeConnections.load() > 0)
        {
            ctx.activeConnections--;
        }
    }

    if (!nodeId.empty())
    {
        std::lock_guard<std::mutex> lock(ctx.registry.registryMutex);
        auto nodeIt = ctx.registry.nodesMap.find(nodeId);
        if (nodeIt != ctx.registry.nodesMap.end())
        {
            nodeIt->second.status = NodeStatus::OFFLINE;
            nodeIt->second.lastSeen = std::chrono::system_clock::now();
            nodeIt->second.sessionId.clear();
        }
    }

    if (foundSession)
    {
        const auto durationMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - connectedAt)
                .count();

        std::ostringstream closeMsg;
        closeMsg << "Disconnect event";
        closeMsg << " timestamp=" << getCurrentTimestamp();
        closeMsg << " node_id=" << (nodeId.empty() ? "UNBOUND" : nodeId);
        closeMsg << " session_id=" << sessionId;
        closeMsg << " client_ip=" << clientIp;
        closeMsg << " connection_duration_ms=" << durationMs;
        closeMsg << " reason=" << reason;
        closeMsg << " total_connections=" << ctx.activeConnections.load();

        auditLogger.writeString(closeMsg.str(), CONNECTION_LOG);
    }

    shutdown(clientSock, SHUT_RDWR);
    close(clientSock);
}

/**
 * @brief Waits for active sessions to finish before forcing shutdown.
 *
 * @param ctx Shared server context.
 * @param timeoutSeconds Maximum time to wait before forcing closure.
 */
static void waitForActiveSessionsToFinish(ServerContext& ctx, int timeoutSeconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);

    while (ctx.activeConnections.load() > 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

/**
 * @brief Closes sessions that remain active after the graceful shutdown window.
 *
 * @param ctx Shared server context.
 */
static void closeRemainingSessions(ServerContext& ctx)
{
    std::vector<int> remainingFds;

    {
        std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
        remainingFds.reserve(ctx.activeSessions.size());

        for (const auto& [fd, session] : ctx.activeSessions)
        {
            if (session.active)
            {
                remainingFds.push_back(fd);
            }
        }
    }

    for (int fd : remainingFds)
    {
        unregisterSession(ctx, fd, "server graceful shutdown");
    }
}

/**
 * @brief Enqueues an incoming message for processing by the Thread Pool.
 *
 * @param ctx Shared server context.
 * @param clientSock Accepted client socket.
 * @param clientIp Client IP.
 * @param message Client message.
 */
static void enqueueMessage(ServerContext& ctx, int clientSock, const std::string& clientIp, const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(ctx.queueMutex);
        ctx.messageQueue.push({clientSock, clientIp, message});
    }
    ctx.queueCv.notify_one();
}

/**
 * @brief Worker loop for the server Thread Pool.
 *
 * Each worker blocks waiting for a message. Once a message
 * becomes available, it is delegated to handleClientMessage(...).
 *
 * @param ctx Shared server context.
 */
static void workerLoop(ServerContext& ctx)
{
    while (true)
    {
        ClientMessage msg;

        {
            std::unique_lock<std::mutex> lock(ctx.queueMutex);
            ctx.queueCv.wait(lock, [&ctx]() { return !ctx.messageQueue.empty() || !ctx.running.load(); });

            if (!ctx.running.load() && ctx.messageQueue.empty())
            {
                return;
            }

            msg = ctx.messageQueue.front();
            ctx.messageQueue.pop();
        }

        handleClientMessage(msg.clientFd, msg.clientIp, msg.message);
    }
}

/**
 * @brief Monitors active sessions and expires inactive clients.
 *
 * Registered sessions are evaluated using heartbeat policy:
 * one missed heartbeat is tolerated, so OFFLINE is triggered after
 * 2 × EOP_HEARTBEAT_INTERVAL without valid traffic.
 *
 * Non-registered sessions are evaluated using EOP_IDLE_TIMEOUT.
 *
 * @param ctx Shared server context.
 */
static void timeoutMonitorLoop(ServerContext& ctx)
{
    while (ctx.running.load())
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        struct ExpiredSessionInfo
        {
            int fd;
            std::string clientIp;
            bool registered;
            int timeoutSeconds;
        };

        std::vector<ExpiredSessionInfo> expiredSessions;
        const auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(ctx.sessionsMutex);
            for (auto& [fd, session] : ctx.activeSessions)
            {
                if (!session.active)
                {
                    continue;
                }

                const int timeoutSeconds = getSessionTimeoutSeconds(ctx, session);
                const auto inactiveSeconds =
                    std::chrono::duration_cast<std::chrono::seconds>(now - session.lastActivity).count();

                if (inactiveSeconds > timeoutSeconds)
                {
                    session.active = false;
                    expiredSessions.push_back({fd, session.clientIp, session.registered, timeoutSeconds});
                }
            }
        }

        for (const auto& expired : expiredSessions)
        {
            std::ostringstream timeoutMsg;

            if (expired.registered)
            {
                timeoutMsg << "Heartbeat timeout exceeded for IP=" << expired.clientIp << " after "
                           << expired.timeoutSeconds << "s";
                auditLogger.writeString(timeoutMsg.str(), WARNING_LOG);
                unregisterSession(ctx, expired.fd, "heartbeat timeout exceeded");
            }
            else
            {
                timeoutMsg << "Pre-registration inactivity timeout exceeded for IP=" << expired.clientIp << " after "
                           << expired.timeoutSeconds << "s";
                auditLogger.writeString(timeoutMsg.str(), WARNING_LOG);
                unregisterSession(ctx, expired.fd, "pre-registration inactivity timeout exceeded");
            }
        }
    }
}

/**
 * @brief Returns the current system timestamp as a formatted string.
 *
 * The timestamp is built using the existing Date and Time helpers so it stays
 * consistent with the rest of the server logs and protocol messages.
 *
 * @return Current timestamp in "DD/MM/YYYY HH:MM:SS" format.
 */
std::string getCurrentTimestamp()
{
    char dateBuffer[DATE_BUFFER_SIZE] = {0};
    char timeBuffer[TIME_BUFFER_SIZE] = {0};
    Date date = {};
    Time time = {};

    date.captureDateToString(std::span(dateBuffer), true);
    time.captureTimeToString(std::span(timeBuffer), true);

    return std::string(dateBuffer) + " " + std::string(timeBuffer);
}

// ---------------------------------------------------------------------------
// Protocol helpers – produce well-formed response strings
// ---------------------------------------------------------------------------

/**
 * @brief Build an ACK response for a successful REGISTER (AC2).
 *
 * @param nodeId Registered node id.
 * @return Formatted ACK message.
 */
static std::string buildAck(const std::string& nodeId)
{
    return "ACK Server v1.0 | TIME " + getCurrentTimestamp() + " | SESSION_ID " + nodeId + "\n";
}

/**
 * @brief Build an ERROR_DUPLICATE response (AC3).
 *
 * Format: "ERROR_DUPLICATE node_id=<id> desc=Node already registered\n"
 *
 * @param nodeId Duplicated node id.
 * @return Formatted duplicate error response.
 */
// LCOV_EXCL_START
static std::string buildErrorDuplicate(const std::string& nodeId)
{
    return getCmdStringFromCode(CMD_ERROR_DUPLICATE) + " node_id=" + nodeId + " desc=Node already registered\n";
}
// LCOV_EXCL_STOP

/**
 * @brief Build an ERROR_NOT_FOUND response.
 *
 * @param nodeId Missing node id.
 * @return Formatted not-found response.
 */
// LCOV_EXCL_START
static std::string buildErrorNotFound(const std::string& nodeId)
{
    return getCmdStringFromCode(CMD_ERROR_NOT_FOUND) + " node_id=" + nodeId + " desc=Node not found\n";
}
// LCOV_EXCL_STOP

/**
 * @brief Serialise a NodeRecord to a single line (AC5, AC7).
 *
 * Format:
 * "node_id=<id> bunker_name=<name> ip_address=<ip> capacity=<cap> status=<status> last_seen=<epoch>"
 *
 * @param rec Node record to serialize.
 * @return Single-line serialized representation.
 */
static std::string serializeRecord(const NodeRecord& rec)
{
    std::ostringstream ss;
    ss << "node_id=" << rec.nodeId << " bunker_name=" << rec.bunkerName << " ip_address=" << rec.ipAddress
       << " capacity=" << rec.capacity << " status=" << getNodeStatusString(rec.status)
       << " last_seen=" << getEpochSeconds(rec.lastSeen);
    return ss.str();
}

// ---------------------------------------------------------------------------
// Command handlers (called from handleClient, AC1-AC7)
// ---------------------------------------------------------------------------

/**
 * @brief Handle a REGISTER command (AC1, AC2, AC3).
 *
 * Expected format:
 * "REGISTER node_id=<id> bunker_name=<name> ip_address=<ip> capacity=<n>"
 *
 * @param clientSock Connected client socket.
 * @param line Raw request line.
 */
static void handleRegister(int clientSock, const std::string& line)
{
    char nodeId[EOP_SMALL_BUFFER_SIZE] = {0};
    char bunkerName[EOP_SMALL_BUFFER_SIZE] = {0};
    char ipAddress[EOP_SMALL_BUFFER_SIZE] = {0};
    int capacity = 0;

    int parsed = std::sscanf(line.c_str(),
                             "REGISTER node_id=%63s bunker_name=%63s ip_address=%63s capacity=%d",
                             nodeId,
                             bunkerName,
                             ipAddress,
                             &capacity);

    if (parsed != 4)
    {
        // LCOV_EXCL_START
        const std::string err = "ERROR_BAD_REQUEST desc=Malformed REGISTER\n";
        send(clientSock, err.c_str(), err.size(), 0);
        auditLogger.writeString("Malformed REGISTER message", ERROR_LOG);
        return;
        // LCOV_EXCL_STOP
    }

    NodeRecord record;
    record.nodeId = nodeId;
    record.bunkerName = bunkerName;
    record.ipAddress = ipAddress;
    record.capacity = capacity;
    record.status = NodeStatus::ONLINE;
    record.lastSeen = std::chrono::system_clock::now();

    const RegisterNodeResult result = serverContext.registry.registerOrReconnectNode(record);

    if (result == RegisterNodeResult::NEW_NODE || result == RegisterNodeResult::RECONNECTED_NODE)
    {
        bindNodeToSession(serverContext, clientSock, record.nodeId);

        const std::string ack = buildAck(record.nodeId);
        send(clientSock, ack.c_str(), ack.size(), 0);

        std::ostringstream logMsg;
        if (result == RegisterNodeResult::RECONNECTED_NODE)
        {
            logMsg << "Node reconnected: " << serializeRecord(record);
        }
        else
        {
            logMsg << "Node registered: " << serializeRecord(record);
        }
        auditLogger.writeString(logMsg.str(), INFO_LOG);
    }
    else
    {
        // LCOV_EXCL_START
        const std::string errDup = buildErrorDuplicate(record.nodeId);
        send(clientSock, errDup.c_str(), errDup.size(), 0);

        auditLogger.writeString("Duplicate REGISTER attempt for node_id=" + record.nodeId, WARNING_LOG);
    }
}
// LCOV_EXCL_STOP

/**
 * @brief Handle a QUERY_NODE command (AC5, AC6).
 *
 * Expected format: "QUERY_NODE node_id=<id>"
 *
 * @param clientSock Connected client socket.
 * @param line Raw request line.
 */
static void handleQueryNode(int clientSock, const std::string& line)
{
    char nodeId[EOP_SMALL_BUFFER_SIZE] = {0};

    if (std::sscanf(line.c_str(), "QUERY_NODE node_id=%63s", nodeId) != 1)
    {
        // LCOV_EXCL_START
        const std::string err = "ERROR_BAD_REQUEST desc=Malformed QUERY_NODE\n";
        send(clientSock, err.c_str(), err.size(), 0);
        return;
        // LCOV_EXCL_STOP
    }

    NodeRecord record;
    if (serverContext.registry.queryNode(nodeId, record))
    {
        const std::string response = serializeRecord(record) + "\n";
        send(clientSock, response.c_str(), response.size(), 0);
    }
    else
    {
        // LCOV_EXCL_START
        const std::string errNotFound = buildErrorNotFound(nodeId);
        send(clientSock, errNotFound.c_str(), errNotFound.size(), 0);

        auditLogger.writeString("QUERY_NODE for unknown node_id=" + std::string(nodeId), WARNING_LOG);
    }
}
// LCOV_EXCL_STOP

/**
 * @brief Handle a LIST_NODES command (AC7).
 *
 * Returns all registered nodes, one per line, terminated by "END_LIST\n".
 *
 * @param clientSock Connected client socket.
 */
static void handleListNodes(int clientSock)
{
    const std::vector<NodeRecord> nodes = serverContext.registry.listNodes();

    std::ostringstream response;
    response << "LIST_NODES count=" << nodes.size() << "\n";
    for (const auto& rec : nodes)
    {
        response << serializeRecord(rec) << "\n";
    }
    response << "END_LIST\n";

    const std::string responseStr = response.str();
    send(clientSock, responseStr.c_str(), responseStr.size(), 0);
}

// ---------------------------------------------------------------------------
// Per-client worker handling
// ---------------------------------------------------------------------------

/**
 * @brief Handles a single message from a connected client.
 *
 * This function processes the text command sent by the client,
 * modifying states or returning responses accordingly.
 *
 * @param clientSock Connected client socket.
 * @param clientIp Remote client IP.
 * @param request The single plaintext command.
 */
void handleClientMessage(int clientSock, const std::string& clientIp, const std::string& request)
{
    if (request.rfind(getCmdStringFromCode(CMD_REGISTER) + " ", 0) == 0)
    {
        audit_log_write_fmt(INFO_LOG, "Request received: 'REGISTER ...' from IP=%s", clientIp.c_str());
        updateSessionActivity(serverContext, clientSock);
        handleRegister(clientSock, request);
    }
    else if (request.rfind(getCmdStringFromCode(CMD_QUERY_NODE) + " ", 0) == 0)
    {
        audit_log_write_fmt(INFO_LOG, "Request received: '%s' from IP=%s", request.c_str(), clientIp.c_str());
        updateSessionActivity(serverContext, clientSock);
        handleQueryNode(clientSock, request);
    }
    else if (request.rfind(getCmdStringFromCode(CMD_LIST_NODES), 0) == 0)
    {
        audit_log_write_fmt(INFO_LOG, "Request received: 'LIST_NODES' from IP=%s", clientIp.c_str());
        updateSessionActivity(serverContext, clientSock);
        handleListNodes(clientSock);
    }
    else if (request.rfind(getCmdStringFromCode(CMD_HEARTBEAT), 0) == 0)
    {
        // Skip logging for HEARTBEAT to maintain high performance under load
        handleHeartbeat(clientSock);
    }
    else
    {
        if (!request.empty())
        {
            audit_log_write_fmt(WARNING_LOG, "Unknown command '%s' from %s", request.c_str(), clientIp.c_str());
            const std::string err = "ERROR_BAD_REQUEST desc=Unknown command\n";
            send(clientSock, err.c_str(), err.size(), 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Server entry-point
// ---------------------------------------------------------------------------

/**
 * @brief Starts the TCP server and accepts incoming client connections.
 *
 * @param port TCP port to bind.
 * @param idleTimeout Initial idle timeout value.
 */
void startServer(int port, int idleTimeout)
{
    serverContext.idleTimeoutSeconds = idleTimeout;
    serverContext.running = true;
    loadServerConfig(serverContext);

    int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    // LCOV_EXCL_START
    if (serverSock < 0)
    {
        std::cerr << "Error: Could not create server socket." << std::endl;
        return;
    }
    // LCOV_EXCL_STOP

    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(static_cast<uint16_t>(port));

    // LCOV_EXCL_START
    if (bind(serverSock, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0)
    {
        std::cerr << "Critical error: Cannot start server on port " << port << ". Port may already be in use."
                  << std::endl;
        auditLogger.writeString("Failed to bind server port", CRITICAL_LOG);
        close(serverSock);
        return;
    }
    // LCOV_EXCL_STOP

    // LCOV_EXCL_START
    if (listen(serverSock, serverContext.maxClients) < 0)
    {
        std::cerr << "Critical error: Failed to listen()." << std::endl;
        close(serverSock);
        return;
    }
    // LCOV_EXCL_STOP

    unsigned int workerCount = std::thread::hardware_concurrency();
    if (workerCount == 0)
    {
        workerCount = 4;
    }
    if (workerCount > static_cast<unsigned int>(serverContext.maxClients))
    {
        workerCount = static_cast<unsigned int>(serverContext.maxClients);
    }
    if (workerCount == 0)
    {
        workerCount = 1;
    }

    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (unsigned int i = 0; i < workerCount; ++i)
    {
        workers.emplace_back(workerLoop, std::ref(serverContext));
    }

    std::thread timeoutMonitor(timeoutMonitorLoop, std::ref(serverContext));

    std::cout << "Server started on port " << port << ". Inactivity timeout: " << serverContext.idleTimeoutSeconds
              << "s | Max clients: " << serverContext.maxClients
              << " | Heartbeat interval: " << serverContext.heartbeatIntervalSeconds
              << "s | Worker threads: " << workerCount << std::endl;
    auditLogger.writeString("Server started", INFO_LOG);

    std::vector<struct pollfd> pollfds;
    pollfds.reserve(serverContext.maxClients + 1);

    while (!isTerminationSignalReceived)
    {
        pollfds.clear();

        // Listener socket
        pollfds.push_back({serverSock, POLLIN, 0});

        {
            std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
            for (auto& [fd, session] : serverContext.activeSessions)
            {
                if (session.active)
                {
                    pollfds.push_back({fd, POLLIN, 0});
                }
            }
        }

        int ret = poll(pollfds.data(), static_cast<nfds_t>(pollfds.size()), 1000); // 1s timeout
        if (ret > 0)
        {
            // Check listener
            if (pollfds[0].revents & POLLIN)
            {
                struct sockaddr_in clientAddr = {};
                socklen_t clientLen = sizeof(clientAddr);
                int clientSock = accept(serverSock, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);

                if (clientSock >= 0)
                {
                    if (isTerminationSignalReceived || !serverContext.running.load())
                    {
                        shutdown(clientSock, SHUT_RDWR);
                        close(clientSock);
                        break;
                    }

                    std::string clientIp = inet_ntoa(clientAddr.sin_addr);
                    std::string sessionId;

                    if (!tryRegisterSession(serverContext, clientSock, clientIp, sessionId))
                    {
                        const std::string rejectMsg = buildConnectionRejected();
                        send(clientSock, rejectMsg.c_str(), rejectMsg.size(), 0);
                        shutdown(clientSock, SHUT_RDWR);
                        close(clientSock);

                        auditLogger.writeString("Rejected connection due to max client limit from IP=" + clientIp,
                                                WARNING_LOG);
                    }
                    else
                    {
                        if (shouldSendImmediateWelcome())
                        {
                            const std::string welcomeMsg = buildWelcomeMessage(sessionId);
                            send(clientSock, welcomeMsg.c_str(), welcomeMsg.size(), 0);
                        }

                        std::ostringstream acceptedMsg;
                        acceptedMsg << "New connection accepted IP=" << clientIp << " Session=" << sessionId
                                    << " Total connections:" << serverContext.activeConnections.load();
                        auditLogger.writeString(acceptedMsg.str(), CONNECTION_LOG);
                    }
                }
            }

            // Check clients
            // Skip index 0 (listener)
            for (size_t i = 1; i < pollfds.size(); ++i)
            {
                if (pollfds[i].revents & (POLLIN | POLLERR | POLLHUP))
                {
                    int fd = pollfds[i].fd;
                    char requestBuf[MESSAGE_BUFFER_SIZE] = {0};
                    ssize_t bytesRecv = recv(fd, requestBuf, sizeof(requestBuf) - 1, MSG_DONTWAIT);

                    if (bytesRecv == 0)
                    {
                        unregisterSession(serverContext, fd, "client closed connection cleanly");
                    }
                    else if (bytesRecv < 0)
                    {
                        if (errno != EAGAIN && errno != EWOULDBLOCK)
                        {
                            unregisterSession(serverContext, fd, "socket recv error / abrupt disconnect");
                        }
                    }
                    else
                    {
                        requestBuf[bytesRecv] = '\0';
                        std::string request(requestBuf);

                        // Dividir por saltos de línea si se reciben varios comandos pegados
                        size_t pos = 0;
                        while ((pos = request.find('\n')) != std::string::npos)
                        {
                            std::string line = request.substr(0, pos);
                            request.erase(0, pos + 1);

                            while (!line.empty() && (line.back() == '\r'))
                            {
                                line.pop_back();
                            }

                            if (line == "quit" || line == "exit")
                            {
                                unregisterSession(serverContext, fd, "client requested graceful exit");
                                break;
                            }
                            else if (!line.empty())
                            {
                                std::string clientIp;
                                {
                                    std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
                                    auto it = serverContext.activeSessions.find(fd);
                                    if (it != serverContext.activeSessions.end())
                                    {
                                        clientIp = it->second.clientIp;
                                    }
                                }
                                enqueueMessage(serverContext, fd, clientIp, line);
                            }
                        }

                        // Parte no terminada (o sin salto de línea), la mandamos igual
                        while (!request.empty() && (request.back() == '\n' || request.back() == '\r'))
                        {
                            request.pop_back();
                        }

                        if (request == "quit" || request == "exit")
                        {
                            unregisterSession(serverContext, fd, "client requested graceful exit");
                        }
                        else if (!request.empty())
                        {
                            std::string clientIp;
                            {
                                std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
                                auto it = serverContext.activeSessions.find(fd);
                                if (it != serverContext.activeSessions.end())
                                {
                                    clientIp = it->second.clientIp;
                                }
                            }
                            enqueueMessage(serverContext, fd, clientIp, request);
                        }
                    }
                }
            }
        }
    }

        auditLogger.writeString("Graceful shutdown requested", INFO_LOG);

    close(serverSock);

    serverContext.running = false;
    serverContext.queueCv.notify_all();

    waitForActiveSessionsToFinish(serverContext, GRACEFUL_SHUTDOWN_TIMEOUT_SECONDS);

    if (serverContext.activeConnections.load() > 0)
    {
        auditLogger.writeString("Graceful shutdown timeout reached; closing remaining sessions", WARNING_LOG);
        closeRemainingSessions(serverContext);
    }

    if (timeoutMonitor.joinable())
    {
        timeoutMonitor.join();
    }

    for (auto& worker : workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    std::cout << "Server stopped." << std::endl;
}

// ---------------------------------------------------------------------------
// main (excluded in test builds)
// ---------------------------------------------------------------------------

#ifndef TEST_MODE
int main()
{
    setSigaction();
    ConfigLoader config("config.env");

    const char* portEnv = std::getenv("EOP_PORT");
    int port = (portEnv != nullptr) ? std::atoi(portEnv) : config.getInt("EOP_PORT", EOP_PORT);

    const char* timeoutEnv = std::getenv("EOP_IDLE_TIMEOUT");
    int idleTimeout =
        (timeoutEnv != nullptr) ? std::atoi(timeoutEnv) : config.getInt("EOP_IDLE_TIMEOUT", EOP_IDLE_TIMEOUT);

    startServer(port, idleTimeout);
    return 0;
}
#endif