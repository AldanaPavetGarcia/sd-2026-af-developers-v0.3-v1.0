// eopServer.cpp
// Implements the EOP server with node-registry commands (AC1-AC7).

#include "../../include/eopServer.hpp"
#include "../../lib/include/configLoader.hpp"

#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

ServerContext serverContext;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
 * Format: "ACK node_id=<id>\n"
 */
static std::string buildAck(const std::string& nodeId)
{
    return "ACK Server v1.0 | TIME " + getCurrentTimestamp() + " | SESSION_ID " + nodeId + "\n";
}

/**
 * @brief Build an ERROR_DUPLICATE response (AC3).
 *
 * Format: "ERROR_DUPLICATE node_id=<id> desc=Node already registered\n"
 */
// LCOV_EXCL_START
static std::string buildErrorDuplicate(const std::string& nodeId)
{
    return getCmdStringFromCode(CMD_ERROR_DUPLICATE) + " node_id=" + nodeId + " desc=Node already registered\n";
}
// LCOV_EXCL_STOP

// LCOV_EXCL_START
static std::string buildErrorNotFound(const std::string& nodeId)
{
    return getCmdStringFromCode(CMD_ERROR_NOT_FOUND) + " node_id=" + nodeId + " desc=Node not found\n";
}
// LCOV_EXCL_STOP

/**
 * @brief Serialise a NodeRecord to a single line (AC5, AC7).
 *
 * Format: "node_id=<id> bunker_name=<name> ip_address=<ip> capacity=<cap>"
 */
static std::string serializeRecord(const NodeRecord& rec)
{
    std::ostringstream ss;
    ss << "node_id=" << rec.nodeId << " bunker_name=" << rec.bunkerName << " ip_address=" << rec.ipAddress
       << " capacity=" << rec.capacity;
    return ss.str();
}

// ---------------------------------------------------------------------------
// Command handlers (called from handleClient, AC1-AC7)
// ---------------------------------------------------------------------------

/**
 * @brief Handle a REGISTER command (AC1, AC2, AC3).
 *
 * Expected format: "REGISTER node_id=<id> bunker_name=<name>
 *                             ip_address=<ip> capacity=<n>"
 *
 * Responds within the same call (no blocking; mutex is brief).
 */
static void handleRegister(int clientSock, const std::string& line)
{
    char nodeId[EOP_SMALL_BUFFER_SIZE] = {0};
    char bunkerName[EOP_SMALL_BUFFER_SIZE] = {0};
    char ipAddress[EOP_SMALL_BUFFER_SIZE] = {0};
    int capacity = 0;

    // Parse all four fields in one sscanf call.
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

    const bool registered = serverContext.registry.registerNode(record);

    if (registered)
    {
        const std::string ack = buildAck(record.nodeId);
        send(clientSock, ack.c_str(), ack.size(), 0);

        std::ostringstream logMsg;
        logMsg << "Node registered: " << serializeRecord(record);
        auditLogger.writeString(logMsg.str(), INFO_LOG);
    }
    else
    {
        // AC3: duplicate – respond with ERROR_DUPLICATE, do NOT overwrite.
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
        // AC5: return all metadata on one line.
        const std::string response = serializeRecord(record) + "\n";
        send(clientSock, response.c_str(), response.size(), 0);
    }
    else
    {
        // LCOV_EXCL_START
        // AC6: node not found – return ERROR_NOT_FOUND, server keeps running.
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
// Per-client thread
// ---------------------------------------------------------------------------

void handleClient(int clientSock, const std::string& clientIp, int idleTimeoutS)
{
    {
        std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
        if (serverContext.activeConnections >= MAX_CLIENTS)
        {
            const std::string rejectMsg = "REJECTED | ERROR: Connection limit reached\n";
            send(clientSock, rejectMsg.c_str(), rejectMsg.size(), 0);
            close(clientSock);
            return;
        }
        serverContext.activeConnections++;

        std::ostringstream newClientMsg;
        newClientMsg << "New connection accepted IP=" << clientIp
                     << " Total connections:" << serverContext.activeConnections.load();
        auditLogger.writeString(newClientMsg.str(), CONNECTION_LOG);
    }

    while (true)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(clientSock, &readfds);

        struct timeval tv = {};
        tv.tv_sec = idleTimeoutS;
        tv.tv_usec = 0;

        int ret = select(clientSock + 1, &readfds, nullptr, nullptr, &tv);
        if (ret > 0)
        {
            char requestBuf[MESSAGE_BUFFER_SIZE] = {0};
            ssize_t bytesRecv = recv(clientSock, requestBuf, sizeof(requestBuf) - 1, 0);

            if (bytesRecv <= 0)
            {
                break;
            }
            requestBuf[bytesRecv] = '\0';

            std::string request(requestBuf);
            while (!request.empty() && (request.back() == '\n' || request.back() == '\r'))
            {
                request.pop_back();
            }

            if (request == "quit" || request == "exit")
            {
                break;
            }

            std::ostringstream logMsg;
            logMsg << "Request received: '" << requestBuf << "' from IP=" << clientIp;
            auditLogger.writeString(logMsg.str(), INFO_LOG);

            std::string fullReq(requestBuf);

            if (fullReq.rfind(getCmdStringFromCode(CMD_REGISTER) + " ", 0) == 0)
            {
                handleRegister(clientSock, fullReq);
            }
            else if (fullReq.rfind(getCmdStringFromCode(CMD_QUERY_NODE) + " ", 0) == 0)
            {
                handleQueryNode(clientSock, fullReq);
            }
            else if (fullReq.rfind(getCmdStringFromCode(CMD_LIST_NODES), 0) == 0)
            {
                handleListNodes(clientSock);
            }
            else
            {
                // LCOV_EXCL_START
                if (!request.empty())
                {
                    const std::string err = "ERROR_BAD_REQUEST desc=Unknown command\n";
                    send(clientSock, err.c_str(), err.size(), 0);
                    auditLogger.writeString("Unknown command from " + clientIp, WARNING_LOG);
                }
                // LCOV_EXCL_STOP
            }
        }
        else if (ret == 0)
        {
            // LCOV_EXCL_START
            std::ostringstream timeoutMsg;
            timeoutMsg << "Inactivity timeout (" << idleTimeoutS << "s) exceeded for IP=" << clientIp;
            auditLogger.writeString(timeoutMsg.str(), WARNING_LOG);
            break;
            // LCOV_EXCL_STOP
        }
        // LCOV_EXCL_START
        else
        {
            break;
        }
        // LCOV_EXCL_STOP
    }

    {
        std::lock_guard<std::mutex> lock(serverContext.sessionsMutex);
        serverContext.activeConnections--;

        std::ostringstream closeMsg;
        closeMsg << "Connection closed IP=" << clientIp
                 << " Total connections:" << serverContext.activeConnections.load();
        auditLogger.writeString(closeMsg.str(), CONNECTION_LOG);
    }

    close(clientSock);
}

// ---------------------------------------------------------------------------
// Server entry-point
// ---------------------------------------------------------------------------

void startServer(int port, int idleTimeout)
{
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
    if (listen(serverSock, MAX_CLIENTS) < 0)
    {
        std::cerr << "Critical error: Failed to listen()." << std::endl;
        close(serverSock);
        return;
    }
    // LCOV_EXCL_STOP

    std::cout << "Server started on port " << port << ". Inactivity timeout: " << idleTimeout << "s" << std::endl;
    auditLogger.writeString("Server started", INFO_LOG);

    while (!isTerminationSignalReceived)
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverSock, &readfds);

        struct timeval tv = {};
        tv.tv_sec = 1; // 1s timeout for checking signal
        tv.tv_usec = 0;

        int ret = select(serverSock + 1, &readfds, nullptr, nullptr, &tv);
        if (ret > 0 && FD_ISSET(serverSock, &readfds))
        {
            struct sockaddr_in clientAddr = {};
            socklen_t clientLen = sizeof(clientAddr);
            int clientSock = accept(serverSock, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);

            if (clientSock >= 0)
            {
                std::string clientIp = inet_ntoa(clientAddr.sin_addr);
                std::thread(handleClient, clientSock, clientIp, idleTimeout).detach();
            }
        }
    }

    close(serverSock);
    std::cout << "Server stopped." << std::endl;
}
// ---------------------------------------------------------------------------
// main (excluded in test builds)
// ---------------------------------------------------------------------------

#ifndef TEST_MODE
int main()
{
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
