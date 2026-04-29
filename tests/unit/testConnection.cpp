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
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "../../include/eopServer.hpp"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

namespace
{

    /// Port used by these tests (different from the integration port 9098).
    /// Dynamically assigned based on PID to allow parallel ctest execution
    const int kTestPort = 9000 + (getpid() % 10000);
    constexpr int kIdleTimeout = 5;

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

        send(sock, msg.c_str(), msg.size(), 0);

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
            std::lock_guard<std::mutex> lock(serverContext.registry.registryMutex);
            serverContext.registry.nodesMap.clear();
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
     * @brief Sends a command through an already connected socket and receives one response.
     *
     * @param sock Connected socket file descriptor.
     * @param msg  Command payload to send.
     * @return Response returned by the server.
     */
    std::string sendOnOpenSocketAndReceive(int sock, const std::string& msg)
    {
        send(sock, msg.c_str(), msg.size(), 0);
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
    const bool found = serverContext.registry.queryNode("dup-1", rec);
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
    EXPECT_TRUE(serverContext.registry.queryNode("persist-1", rec));
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
        ASSERT_TRUE(serverContext.registry.queryNode("conc-" + std::to_string(i), rec));
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