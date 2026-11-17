/**
 * @file testConnection.cpp
 * @brief Integration tests for EOP server – covers AC1-AC7 of the
 *        node-registry feature plus the legacy session path.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "../../include/eopServer.hpp"
#include "../../include/eop_client.h"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

namespace
{

    /// Port used by these tests (different from the integration port 9098).
    /// Dynamically assigned based on PID to allow parallel ctest execution
    const int kTestPort = 9000 + (getpid() % 10000);
    constexpr int kIdleTimeout = 5;

    /// Appends the '\n' that ADR-003 uses to frame messages, unless the caller already sent one.
    /// The server buffers anything without it as a partial message (see issue #55), so a command
    /// missing its terminator would simply never be dispatched.
    std::string withProtocolTerminator(const std::string& msg)
    {
        if (!msg.empty() && msg.back() == '\n')
        {
            return msg;
        }
        return msg + "\n";
    }

    /// One-shot: connect, send msg, receive response, close.  Returns the response.
    std::string sendAndReceive(const std::string& msg, int port = kTestPort)
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            return "";
        }

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            close(sock);
            return "";
        }

        const std::string framed = withProtocolTerminator(msg);
        send(sock, framed.c_str(), framed.size(), 0);

        char buf[1024] = {0};
        ssize_t bytes = recv(sock, buf, sizeof(buf) - 1, 0);
        close(sock);

        if (bytes <= 0)
        {
            return "";
        }
        return std::string(buf, static_cast<std::size_t>(bytes));
    }

    // ---------------------------------------------------------------------------
    // Test fixture: spins up the server once for the whole suite.
    // ---------------------------------------------------------------------------
    class EopServerTest : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            isTerminationSignalReceived = false;
            // Start the server in a background thread; it runs indefinitely.
            serverThread = std::thread([]() { startServer(kTestPort, kIdleTimeout); });
            // Give the OS a moment to bind and enter accept().
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        static void TearDownTestSuite()
        {
            isTerminationSignalReceived = true;
            // Signal the server to wake up from select() by connecting to it.
            sendAndReceive("WAKEUP", kTestPort);
            if (serverThread.joinable())
            {
                serverThread.join();
            }
        }

        void SetUp() override
        {
            // Reset registry between tests so they stay independent.
            serverContext.registry.clear();
        }

        static std::thread serverThread;
    };

    /**
     * @brief Opens a TCP client socket connected to the test server.
     *
     * @param port Target port. Defaults to the shared test server port.
     * @return Connected socket fd, or -1 on error.
     */
    int openClientSocket(int port = kTestPort)
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            return -1;
        }

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            close(sock);
            return -1;
        }

        return sock;
    }

    /**
     * @brief Receives a single response message from an already connected socket.
     *
     * @param sock Connected socket file descriptor.
     * @return Response payload, or empty string if recv fails or the socket is closed.
     */
    std::string receiveFromSocket(int sock)
    {
        char buf[1024] = {0};
        ssize_t bytes = recv(sock, buf, sizeof(buf) - 1, 0);
        if (bytes <= 0)
        {
            return "";
        }
        return std::string(buf, static_cast<std::size_t>(bytes));
    }

    /**
     * @brief Receives a single response message with a bounded timeout.
     *
     * @param sock Connected socket file descriptor.
     * @param timeoutMs Timeout in milliseconds.
     * @return Response payload, or empty string on timeout/error.
     */
    std::string receiveFromSocketWithTimeout(int sock, int timeoutMs)
    {
        struct pollfd pfd = {};
        pfd.fd = sock;
        pfd.events = POLLIN;
        pfd.revents = 0;

        const int pollResult = poll(&pfd, 1, timeoutMs);
        if (pollResult <= 0 || (pfd.revents & POLLIN) == 0)
        {
            return "";
        }

        return receiveFromSocket(sock);
    }

    /**
     * @brief Sends a command through an already connected socket and receives one response.
     *
     * @param sock Connected socket file descriptor.
     * @param msg  Command payload to send.
     * @return Response returned by the server.
     */
    std::string sendOnOpenSocketAndReceive(int sock, const std::string& msg)
    {
        const std::string framed = withProtocolTerminator(msg);
        send(sock, framed.c_str(), framed.size(), 0);
        return receiveFromSocket(sock);
    }

    /**
     * @brief Polls QUERY_NODE until the expected status is observed or retries are exhausted.
     *
     * @param nodeId          Node identifier to query.
     * @param expectedStatus  Expected textual status (e.g. "ONLINE", "OFFLINE").
     * @param retries         Maximum number of polling attempts.
     * @param delayMs         Delay between attempts in milliseconds.
     * @return true if the expected status is observed, false otherwise.
     */
    bool
    waitForNodeStatus(const std::string& nodeId, const std::string& expectedStatus, int retries = 20, int delayMs = 100)
    {
        for (int i = 0; i < retries; ++i)
        {
            const std::string response = sendAndReceive("QUERY_NODE node_id=" + nodeId);
            if (response.find("status=" + expectedStatus) != std::string::npos)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
        return false;
    }

    std::thread EopServerTest::serverThread;

} // namespace

// ---------------------------------------------------------------------------
// REGISTER → ACK contains the node_id
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, RegisterReturnsAck)
{
    const std::string response =
        sendAndReceive("REGISTER node_id=bunker-1 bunker_name=Alpha ip_address=10.0.0.1 capacity=50");

    EXPECT_NE(response.find("ACK"), std::string::npos) << "Expected ACK, got: " << response;
    EXPECT_NE(response.find("bunker-1"), std::string::npos) << "ACK should echo back the node_id, got: " << response;
}

// ---------------------------------------------------------------------------
// Duplicate REGISTER → ERROR_DUPLICATE; existing entry unchanged
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, DuplicateRegisterReturnsErrorDuplicate)
{
    // First registration should succeed.
    const std::string first = sendAndReceive("REGISTER node_id=dup-1 bunker_name=Beta ip_address=10.0.0.2 capacity=30");
    EXPECT_NE(first.find("ACK"), std::string::npos) << "First: " << first;

    // Second registration with the same node_id must be rejected.
    const std::string second =
        sendAndReceive("REGISTER node_id=dup-1 bunker_name=Gamma ip_address=10.0.0.3 capacity=99");
    EXPECT_NE(second.find("ERROR_DUPLICATE"), std::string::npos) << "Expected ERROR_DUPLICATE, got: " << second;

    // The original entry must be preserved (capacity=30, not 99).
    NodeRecord rec;
    const bool found = serverContext.registry.queryNode("dup-1", rec) == RegistryResult::OK;
    ASSERT_TRUE(found);
    EXPECT_EQ(rec.capacity, 30) << "Original entry must not be overwritten";
    EXPECT_EQ(rec.bunkerName, "Beta");
}

// ---------------------------------------------------------------------------
// Registry persists after the client socket closes
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, RegistryPersistsAfterClientDisconnects)
{
    sendAndReceive("REGISTER node_id=persist-1 bunker_name=Delta ip_address=10.0.0.4 capacity=10");

    // Socket is already closed by sendAndReceive.  Verify the entry is still there.
    NodeRecord rec;
    EXPECT_EQ(serverContext.registry.queryNode("persist-1", rec), RegistryResult::OK);
    EXPECT_EQ(rec.nodeId, "persist-1");
}

// ---------------------------------------------------------------------------
// QUERY_NODE for an existing node returns all metadata
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, QueryNodeReturnsAllMetadata)
{
    sendAndReceive("REGISTER node_id=q-1 bunker_name=Echo ip_address=192.168.1.1 capacity=200");

    const std::string response = sendAndReceive("QUERY_NODE node_id=q-1");

    EXPECT_NE(response.find("node_id=q-1"), std::string::npos) << response;
    EXPECT_NE(response.find("bunker_name=Echo"), std::string::npos) << response;
    EXPECT_NE(response.find("ip_address=192.168.1.1"), std::string::npos) << response;
    EXPECT_NE(response.find("capacity=200"), std::string::npos) << response;
}

// ---------------------------------------------------------------------------
// QUERY_NODE for an unknown node → ERROR_NOT_FOUND (server stays up)
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, QueryUnknownNodeReturnsErrorNotFound)
{
    const std::string response = sendAndReceive("QUERY_NODE node_id=nonexistent-xyz");

    EXPECT_NE(response.find("ERROR_NOT_FOUND"), std::string::npos) << "Expected ERROR_NOT_FOUND, got: " << response;

    // Verify the server is still responsive after returning the error.
    const std::string followUp =
        sendAndReceive("REGISTER node_id=after-err bunker_name=Foxtrot ip_address=10.0.0.5 capacity=1");
    EXPECT_NE(followUp.find("ACK"), std::string::npos) << "Server must remain functional after ERROR_NOT_FOUND";
}

TEST_F(EopServerTest, MalformedRegisterReturnsError)
{
    const std::string response = sendAndReceive("REGISTER node_id=only-id");
    EXPECT_NE(response.find("ERROR_BAD_REQUEST desc=Malformed REGISTER"), std::string::npos) << response;
}

TEST_F(EopServerTest, MalformedQueryNodeReturnsError)
{
    const std::string response = sendAndReceive("QUERY_NODE ");
    EXPECT_NE(response.find("ERROR_BAD_REQUEST desc=Malformed QUERY_NODE"), std::string::npos) << response;
}

TEST_F(EopServerTest, UnknownCommandReturnsError)
{
    const std::string response = sendAndReceive("BLABLA_COMMAND arg=1");
    EXPECT_NE(response.find("ERROR_BAD_REQUEST desc=Unknown command"), std::string::npos) << response;
}

TEST_F(EopServerTest, AnalyzeGraphReturnsServiceUnavailableWhenNoHpcEngineIsRegistered)
{
    const std::string response = sendAndReceive(
        R"({"type":"ANALYZE_GRAPH","request_id":"test-offline-engine","payload":{"nodes":["A"],"edges":[],"algorithms":["connected_components"]}})");

    EXPECT_NE(response.find(getCmdStringFromCode(CMD_ERROR_SERVICE_UNAVAILABLE)), std::string::npos)
        << "Expected ERROR_SERVICE_UNAVAILABLE, got: " << response;
    EXPECT_NE(response.find("HPC engine unavailable"), std::string::npos) << response;
}

TEST_F(EopServerTest, ServerBindFailureTest)
{
    // Port 1 is privileged, binding will fail and hit the error branch in startServer
    startServer(1, 1);
}

// ---------------------------------------------------------------------------
// LIST_NODES returns all registered nodes
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, ListNodesReturnsAllRegistered)
{
    sendAndReceive("REGISTER node_id=list-1 bunker_name=Golf ip_address=10.1.0.1 capacity=5");
    sendAndReceive("REGISTER node_id=list-2 bunker_name=Hotel ip_address=10.1.0.2 capacity=7");

    const std::string response = sendAndReceive("LIST_NODES");

    EXPECT_NE(response.find("LIST_NODES"), std::string::npos) << response;
    EXPECT_NE(response.find("list-1"), std::string::npos) << response;
    EXPECT_NE(response.find("list-2"), std::string::npos) << response;
    EXPECT_NE(response.find("END_LIST"), std::string::npos) << response;
}

// ---------------------------------------------------------------------------
// EXIT/QUIT command case
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, ExitCommandClosesConnection)
{
    const std::string response = sendAndReceive("exit");
    // sendAndReceive expects a response, but exit might not send one or might close immediately.
    // Our sendAndReceive returns empty string if recv fails.
    EXPECT_EQ(response, "");
}

// ---------------------------------------------------------------------------
// Inactivity timeout case
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, InactivityTimeoutClosesConnection)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0) << "Failed to create socket";
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(kTestPort));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    // Wait for timeout (kIdleTimeout is 5s)
    std::this_thread::sleep_for(std::chrono::seconds(kIdleTimeout + 1));

    char buf[10];
    ssize_t bytes = recv(sock, buf, sizeof(buf), 0);
    EXPECT_LE(bytes, 0) << "Connection should be closed by timeout";
    close(sock);
}

// ---------------------------------------------------------------------------
// Legacy – legacy REQ_SESSION path still works (backward compatibility)
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, Reaches100ConnectionsSuccessfully)
{
    const int iterations = 100;

    for (int i = 1; i <= iterations; ++i)
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(sock, 0) << "Error creating socket in iteration " << i;

        struct sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(static_cast<uint16_t>(kTestPort));
        inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

        int res = connect(sock, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr));
        ASSERT_EQ(res, 0) << "Error connecting in iteration " << i;

        // Send a REGISTER request.
        const std::string reqMsg =
            "REGISTER node_id=test_" + std::to_string(i) + " bunker_name=Test ip_address=10.0.0.1 capacity=1\n";
        send(sock, reqMsg.c_str(), reqMsg.size(), 0);

        char buffer[256] = {0};
        ssize_t bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
        ASSERT_GT(bytes, 0) << "No response received in iteration " << i;
        buffer[bytes] = '\0';

        const bool valid = (strstr(buffer, "ACK") != nullptr) || (strstr(buffer, "REJECTED") != nullptr);
        EXPECT_TRUE(valid) << "Unexpected message: " << buffer;

        close(sock);
    }
}

// ---------------------------------------------------------------------------
// Multiple concurrent clients can register successfully
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, ConcurrentClientsCanRegisterSuccessfully)
{
    constexpr int kClients = 10;

    std::atomic<int> readyCount {0};
    std::atomic<bool> startFlag {false};

    std::vector<std::thread> threads;
    std::vector<std::string> responses(kClients);

    for (int i = 0; i < kClients; ++i)
    {
        threads.emplace_back(
            [&, i]()
            {
                readyCount++;
                while (!startFlag.load())
                {
                    std::this_thread::yield();
                }

                responses[i] = sendAndReceive("REGISTER node_id=conc-" + std::to_string(i) +
                                              " bunker_name=Thread ip_address=10.0.0." + std::to_string(i + 1) +
                                              " capacity=" + std::to_string(i + 1));
            });
    }

    while (readyCount.load() < kClients)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    startFlag = true;

    for (auto& t : threads)
    {
        t.join();
    }

    for (int i = 0; i < kClients; ++i)
    {
        EXPECT_NE(responses[i].find("ACK"), std::string::npos)
            << "Thread " << i << " expected ACK, got: " << responses[i];
    }

    for (int i = 0; i < kClients; ++i)
    {
        NodeRecord rec;
        ASSERT_EQ(serverContext.registry.queryNode("conc-" + std::to_string(i), rec), RegistryResult::OK);
        EXPECT_EQ(rec.nodeId, "conc-" + std::to_string(i));
    }
}

// ---------------------------------------------------------------------------
// Abrupt disconnect transitions node status to OFFLINE
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, AbruptDisconnectMarksNodeOffline)
{
    const std::string nodeId = "offline-1";

    int sock = openClientSocket();
    ASSERT_GE(sock, 0);

    const std::string ack = sendOnOpenSocketAndReceive(
        sock, "REGISTER node_id=" + nodeId + " bunker_name=Delta ip_address=10.0.0.4 capacity=10");
    ASSERT_NE(ack.find("ACK"), std::string::npos) << ack;

    // Disconnect without sending exit/quit.
    close(sock);

    EXPECT_TRUE(waitForNodeStatus(nodeId, "OFFLINE")) << "Expected node to become OFFLINE after abrupt disconnect";
}

// ---------------------------------------------------------------------------
// SDK clean disconnect is detected via recv() == 0 and marks node OFFLINE
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, SdkCleanDisconnectMarksNodeOffline)
{
    const std::string nodeId = "us107-sdk-clean-disconnect";
    const std::string payload = "node_id=" + nodeId + " bunker_name=SdkClean ip_address=127.0.0.1 capacity=10";

    eop_client_t* client = eop_connect("127.0.0.1", static_cast<uint16_t>(kTestPort), 1000);
    ASSERT_NE(client, nullptr) << eop_last_error();

    eop_response_t* response =
        eop_send_command(client, 0, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), 1000);

    ASSERT_NE(response, nullptr) << eop_last_error();

    const std::string responsePayload(reinterpret_cast<const char*>(response->payload), response->payload_len);

    EXPECT_EQ(response->status_code, EOP_OK);
    EXPECT_NE(responsePayload.find("ACK"), std::string::npos) << responsePayload;
    EXPECT_NE(responsePayload.find(nodeId), std::string::npos) << responsePayload;

    eop_response_free(response);

    eop_disconnect(client);

    EXPECT_TRUE(waitForNodeStatus(nodeId, "OFFLINE")) << "Expected node to become OFFLINE after SDK clean disconnect";
}

// ---------------------------------------------------------------------------
// Abrupt disconnect of one client does not affect other connected clients
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, AbruptDisconnectDoesNotAffectOtherClients)
{
    const std::string deadNodeId = "dead-1";
    const std::string aliveNodeId = "alive-1";

    int deadSock = openClientSocket();
    ASSERT_GE(deadSock, 0);

    int aliveSock = openClientSocket();
    ASSERT_GE(aliveSock, 0);

    const std::string deadAck = sendOnOpenSocketAndReceive(
        deadSock, "REGISTER node_id=" + deadNodeId + " bunker_name=Dead ip_address=10.0.0.10 capacity=10");
    ASSERT_NE(deadAck.find("ACK"), std::string::npos) << deadAck;

    const std::string aliveAck = sendOnOpenSocketAndReceive(
        aliveSock, "REGISTER node_id=" + aliveNodeId + " bunker_name=Alive ip_address=10.0.0.11 capacity=11");
    ASSERT_NE(aliveAck.find("ACK"), std::string::npos) << aliveAck;

    // Abruptly terminate one client connection.
    close(deadSock);

    // The disconnected node must become OFFLINE.
    EXPECT_TRUE(waitForNodeStatus(deadNodeId, "OFFLINE")) << "Expected abruptly disconnected node to become OFFLINE";

    // The other client must continue operating normally.
    const std::string queryAlive = sendOnOpenSocketAndReceive(aliveSock, "QUERY_NODE node_id=" + aliveNodeId);

    EXPECT_NE(queryAlive.find("node_id=" + aliveNodeId), std::string::npos) << queryAlive;
    EXPECT_NE(queryAlive.find("status=ONLINE"), std::string::npos)
        << "Expected surviving client node to remain ONLINE, got: " << queryAlive;

    // The surviving client must also be able to observe the OFFLINE state of the dead node.
    const std::string queryDead = sendOnOpenSocketAndReceive(aliveSock, "QUERY_NODE node_id=" + deadNodeId);

    EXPECT_NE(queryDead.find("node_id=" + deadNodeId), std::string::npos) << queryDead;
    EXPECT_NE(queryDead.find("status=OFFLINE"), std::string::npos)
        << "Expected dead node to be visible as OFFLINE, got: " << queryDead;

    close(aliveSock);
}

// ---------------------------------------------------------------------------
// Reconnection restores ONLINE status for the same node
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, ReconnectionRestoresOnlineStatus)
{
    const std::string nodeId = "reconnect-1";
    const std::string registerCmd = "REGISTER node_id=" + nodeId + " bunker_name=Alpha ip_address=10.0.0.1 capacity=50";

    int sock1 = openClientSocket();
    ASSERT_GE(sock1, 0);

    const std::string firstAck = sendOnOpenSocketAndReceive(sock1, registerCmd);
    ASSERT_NE(firstAck.find("ACK"), std::string::npos) << firstAck;

    close(sock1);

    ASSERT_TRUE(waitForNodeStatus(nodeId, "OFFLINE")) << "Expected node to become OFFLINE before reconnection";

    int sock2 = openClientSocket();
    ASSERT_GE(sock2, 0);

    const std::string secondAck = sendOnOpenSocketAndReceive(sock2, registerCmd);
    ASSERT_NE(secondAck.find("ACK"), std::string::npos) << secondAck;

    EXPECT_TRUE(waitForNodeStatus(nodeId, "ONLINE")) << "Expected node to become ONLINE again after reconnection";

    close(sock2);
}

// ---------------------------------------------------------------------------
// Max client limit is enforced for incoming connections
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, MaxClientsLimitIsEnforced)
{
    const int previousMaxClients = serverContext.maxClients;
    serverContext.maxClients = 2;

    int sock1 = openClientSocket();
    ASSERT_GE(sock1, 0);

    int sock2 = openClientSocket();
    ASSERT_GE(sock2, 0);

    // Give the accept loop a moment to register both sessions.
    std::this_thread::sleep_for(std::chrono::seconds(2));

    int sock3 = openClientSocket();
    ASSERT_GE(sock3, 0);

    const std::string thirdResponse = receiveFromSocket(sock3);
    EXPECT_NE(thirdResponse.find("REJECTED"), std::string::npos)
        << "Expected third client to be rejected, got: " << thirdResponse;

    close(sock1);
    close(sock2);
    close(sock3);

    serverContext.maxClients = previousMaxClients;
}

// ---------------------------------------------------------------------------
// HEARTBEAT traffic keeps the node ONLINE
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, HeartbeatKeepsNodeOnline)
{
    const std::string nodeId = "heartbeat-1";
    const std::string registerCmd = "REGISTER node_id=" + nodeId + " bunker_name=Pulse ip_address=10.0.0.9 capacity=9";

    int sock = openClientSocket();
    ASSERT_GE(sock, 0);

    const std::string ack = sendOnOpenSocketAndReceive(sock, registerCmd);
    ASSERT_NE(ack.find("ACK"), std::string::npos) << ack;

    for (int i = 0; i < kIdleTimeout + 2; ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        const std::string hbResponse = sendOnOpenSocketAndReceive(sock, getCmdStringFromCode(CMD_HEARTBEAT));

        EXPECT_NE(hbResponse.find("ACK"), std::string::npos) << "Expected HEARTBEAT ACK, got: " << hbResponse;
    }

    EXPECT_TRUE(waitForNodeStatus(nodeId, "ONLINE"))
        << "Expected node to remain ONLINE while heartbeats are being sent";

    close(sock);
}

// ---------------------------------------------------------------------------
// Full heartbeat -> abrupt disconnect -> OFFLINE -> reconnect -> ONLINE cycle
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, HeartbeatDisconnectAndReconnectFullCycle)
{
    const std::string nodeId = "hb-cycle-1";
    const std::string registerCmd =
        "REGISTER node_id=" + nodeId + " bunker_name=Cycle ip_address=10.0.0.20 capacity=20";

    int sock1 = openClientSocket();
    ASSERT_GE(sock1, 0);

    // 1) REGISTER -> ONLINE
    const std::string firstAck = sendOnOpenSocketAndReceive(sock1, registerCmd);
    ASSERT_NE(firstAck.find("ACK"), std::string::npos) << firstAck;
    ASSERT_TRUE(waitForNodeStatus(nodeId, "ONLINE")) << "Expected node to become ONLINE after REGISTER";

    // 2) Establish heartbeat traffic so the session is clearly alive
    for (int i = 0; i < 2; ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        const std::string hbResponse = sendOnOpenSocketAndReceive(sock1, getCmdStringFromCode(CMD_HEARTBEAT));

        ASSERT_NE(hbResponse.find("ACK"), std::string::npos) << "Expected HEARTBEAT ACK, got: " << hbResponse;
    }

    // 3) Kill client process equivalent for this test: abrupt socket close
    close(sock1);

    // 4) Verify OFFLINE
    ASSERT_TRUE(waitForNodeStatus(nodeId, "OFFLINE")) << "Expected node to become OFFLINE after abrupt disconnect";

    // 5) Reconnect and re-register
    int sock2 = openClientSocket();
    ASSERT_GE(sock2, 0);

    const std::string secondAck = sendOnOpenSocketAndReceive(sock2, registerCmd);
    ASSERT_NE(secondAck.find("ACK"), std::string::npos) << secondAck;

    // 6) Verify ONLINE again
    EXPECT_TRUE(waitForNodeStatus(nodeId, "ONLINE")) << "Expected node to become ONLINE again after reconnection";

    close(sock2);
}

// ---------------------------------------------------------------------------
// ANALYZE_GRAPH is forwarded to a registered HPC Engine
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, AnalyzeGraphForwardsRequestToRegisteredHpcEngine)
{
    int engineSock = openClientSocket();
    ASSERT_GE(engineSock, 0);

    const std::string engineAck = sendOnOpenSocketAndReceive(
        engineSock, "REGISTER node_id=hpc-engine bunker_name=HPC ip_address=127.0.0.1 capacity=1");

    ASSERT_NE(engineAck.find("ACK"), std::string::npos) << engineAck;

    std::string clientResponse;
    std::thread clientThread(
        [&clientResponse]()
        {
            clientResponse = sendAndReceive(
                R"({"type":"ANALYZE_GRAPH","request_id":"req-dispatch","payload":{"nodes":["A"],"edges":[],"algorithms":["connected_components"]}})");
        });

    const std::string forwardedRequest = receiveFromSocketWithTimeout(engineSock, 2000);

    EXPECT_NE(forwardedRequest.find(R"("type":"ANALYZE_GRAPH")"), std::string::npos) << forwardedRequest;
    EXPECT_NE(forwardedRequest.find("req-dispatch"), std::string::npos) << forwardedRequest;

    const std::string engineResponse =
        R"({"type":"RESPONSE","status":"ACK","request_id":"req-dispatch","result":{"ok":true}})"
        "\n";

    send(engineSock, engineResponse.c_str(), engineResponse.size(), 0);

    clientThread.join();

    EXPECT_NE(clientResponse.find(R"("status":"ACK")"), std::string::npos) << clientResponse;
    EXPECT_NE(clientResponse.find("req-dispatch"), std::string::npos) << clientResponse;

    close(engineSock);
}

// ---------------------------------------------------------------------------
// ANALYZE_GRAPH returns timeout when the registered HPC Engine stalls
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, AnalyzeGraphReturnsTimeoutWhenHpcEngineStalls)
{
    setenv("EOP_HPC_TIMEOUT_MS", "100", 1);

    int engineSock = openClientSocket();
    ASSERT_GE(engineSock, 0);

    const std::string engineAck = sendOnOpenSocketAndReceive(
        engineSock, "REGISTER node_id=hpc-engine bunker_name=HPC ip_address=127.0.0.1 capacity=1");

    ASSERT_NE(engineAck.find("ACK"), std::string::npos) << engineAck;

    const std::string clientResponse = sendAndReceive(
        R"({"type":"ANALYZE_GRAPH","request_id":"req-timeout","payload":{"nodes":["A"],"edges":[],"algorithms":["connected_components"]}})");

    EXPECT_NE(clientResponse.find(getCmdStringFromCode(CMD_ERROR_HPC_TIMEOUT)), std::string::npos) << clientResponse;
    EXPECT_NE(clientResponse.find("HPC engine timeout"), std::string::npos) << clientResponse;

    close(engineSock);
    unsetenv("EOP_HPC_TIMEOUT_MS");
}

// ---------------------------------------------------------------------------
// ANALYZE_GRAPH: the HPC Engine drops the connection after receiving the request
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, AnalyzeGraphReturnsServiceUnavailableWhenHpcEngineDisconnectsMidDispatch)
{
    int engineSock = openClientSocket();
    ASSERT_GE(engineSock, 0);

    const std::string engineAck = sendOnOpenSocketAndReceive(
        engineSock, "REGISTER node_id=hpc-engine bunker_name=HPC ip_address=127.0.0.1 capacity=1");
    ASSERT_NE(engineAck.find("ACK"), std::string::npos) << engineAck;

    const std::string request =
        R"({"type":"ANALYZE_GRAPH","request_id":"req-drop","payload":{"nodes":["A"],"edges":[],"algorithms":["connected_components"]}})";

    std::string clientResponse;
    std::thread clientThread([&clientResponse, &request]() { clientResponse = sendAndReceive(request); });

    // The engine crashes while working on the request: it read it but never answers.
    const std::string forwardedRequest = receiveFromSocketWithTimeout(engineSock, 2000);
    EXPECT_NE(forwardedRequest.find("req-drop"), std::string::npos) << forwardedRequest;
    close(engineSock);

    clientThread.join();

    // The client gets a structured error at once instead of waiting for EOP_HPC_TIMEOUT_MS...
    EXPECT_NE(clientResponse.find(getCmdStringFromCode(CMD_ERROR_SERVICE_UNAVAILABLE)), std::string::npos)
        << clientResponse;
    // ...and the dead engine is dropped, so the next request is not dispatched to it.
    EXPECT_TRUE(waitForNodeStatus("hpc-engine", "OFFLINE"));
    EXPECT_NE(sendAndReceive(request).find(getCmdStringFromCode(CMD_ERROR_SERVICE_UNAVAILABLE)), std::string::npos);
}

// ---------------------------------------------------------------------------
// US-104: a registered node that stops sending heartbeats is marked OFFLINE
// ---------------------------------------------------------------------------
TEST_F(EopServerTest, SilentRegisteredNodeGoesOfflineAfterHeartbeatTimeout)
{
    const std::string nodeId = "silent-node";

    int sock = openClientSocket();
    ASSERT_GE(sock, 0);

    const std::string ack = sendOnOpenSocketAndReceive(
        sock, "REGISTER node_id=" + nodeId + " bunker_name=Silent ip_address=10.0.0.9 capacity=1");
    ASSERT_NE(ack.find("ACK"), std::string::npos) << ack;
    ASSERT_TRUE(waitForNodeStatus(nodeId, "ONLINE"));

    // The socket stays open but nothing else is sent: only the heartbeat timeout (twice the
    // default 5 s interval) can detect this node, unlike a disconnect.
    constexpr int kHeartbeatTimeoutSeconds = 10;
    constexpr int kPollDelayMs = 500;
    EXPECT_TRUE(
        waitForNodeStatus(nodeId, "OFFLINE", (kHeartbeatTimeoutSeconds + 5) * 1000 / kPollDelayMs, kPollDelayMs));

    char buf[16];
    EXPECT_LE(recv(sock, buf, sizeof(buf), 0), 0) << "the server should close the silent session";
    close(sock);
}

// ---------------------------------------------------------------------------
// v0.1 bug (#71): a command left without '\n' when the client closes is answered to that client,
// never to whoever gets the same fd next
// ---------------------------------------------------------------------------
namespace
{
    /// Sends `command` without its terminator, half-closes, and reads until the server closes.
    std::string sendUnterminatedAndHalfClose(const std::string& command)
    {
        int sock = openClientSocket();
        if (sock < 0)
        {
            return "";
        }
        send(sock, command.c_str(), command.size(), 0);
        shutdown(sock, SHUT_WR);

        constexpr int kReadTimeoutMs = 5000;
        std::string all;
        for (;;)
        {
            const std::string chunk = receiveFromSocketWithTimeout(sock, kReadTimeoutMs);
            if (chunk.empty())
            {
                break;
            }
            all += chunk;
        }
        close(sock);
        return all;
    }
} // namespace

TEST_F(EopServerTest, UnterminatedCommandBeforeHalfCloseIsAnsweredThenClosed)
{
    const std::string response = sendUnterminatedAndHalfClose("LIST_NODES");
    EXPECT_NE(response.find("END_LIST"), std::string::npos)
        << "a half-closed client still gets its answer: " << response;
}

TEST_F(EopServerTest, UnterminatedCommandsBeforeCloseNeverReachAnotherClient)
{
    // Many clients close with a pending command while others keep connecting: with the old code
    // the fd was closed before the answer and could be handed to one of the new clients.
    constexpr int kClients = 24;
    std::vector<std::string> responses(kClients);
    std::vector<std::thread> threads;
    threads.reserve(kClients);
    for (int i = 0; i < kClients; ++i)
    {
        threads.emplace_back(
            [i, &responses]()
            { responses[i] = sendUnterminatedAndHalfClose("QUERY_NODE node_id=fd-" + std::to_string(i)); });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    for (int i = 0; i < kClients; ++i)
    {
        const std::string own = "node_id=fd-" + std::to_string(i) + " ";
        EXPECT_NE(responses[i].find(own), std::string::npos) << "client " << i << " got: " << responses[i];
        for (int j = 0; j < kClients; ++j)
        {
            if (j != i)
            {
                EXPECT_EQ(responses[i].find("node_id=fd-" + std::to_string(j) + " "), std::string::npos)
                    << "client " << i << " got the answer meant for client " << j;
            }
        }
    }
}
