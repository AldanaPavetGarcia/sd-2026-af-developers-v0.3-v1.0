/**
 * @file testLoad.cpp
 * @brief High load test for EOP server.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "../../include/eopServer.hpp"

namespace
{
    // Use a unique port for this test to avoid conflicts
    const int kLoadTestPort = 11000 + (getpid() % 5000);
    constexpr int kNumClients = 20;
    constexpr int kRequestsPerClient = 50;
    constexpr rlim_t kRequiredFds = 1024;

    class EopLoadTest : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            // Increase FD limit to handle both client-side and server-side sockets
            struct rlimit rl;
            if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
            {
                if (rl.rlim_cur < kRequiredFds)
                {
                    rl.rlim_cur = kRequiredFds;
                    if (rl.rlim_max < rl.rlim_cur)
                    {
                        rl.rlim_max = rl.rlim_cur;
                    }

                    if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
                    {
                        perror("setrlimit");
                    }
                }

                struct rlimit verify;
                if (getrlimit(RLIMIT_NOFILE, &verify) != 0 || verify.rlim_cur < kRequiredFds)
                {
                    GTEST_SKIP() << "RLIMIT_NOFILE too low for CI load test";
                }
            }

            isTerminationSignalReceived = false;

            // Relax runtime configuration for this stress test
            setenv("EOP_MAX_CLIENTS", "1200", 1);
            setenv("EOP_HEARTBEAT_INTERVAL", "30", 1);
            setenv("EOP_IDLE_TIMEOUT", "60", 1);
            serverContext.maxClients = 1200;
            serverContext.heartbeatIntervalSeconds = 30;
            serverContext.idleTimeoutSeconds = 60;

            // Start the server in a background thread
            serverThread = std::thread([]() { startServer(kLoadTestPort, 60); });

            // Give the server a moment to start
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        static void TearDownTestSuite()
        {
            isTerminationSignalReceived = true;

            // Wake up the server from select()/poll() by connecting to it
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock >= 0)
            {
                struct sockaddr_in addr = {};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<uint16_t>(kLoadTestPort));
                inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
                if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0)
                {
                    const char* msg = "WAKEUP\n";
                    send(sock, msg, strlen(msg), 0);
                }
                close(sock);
            }

            if (serverThread.joinable())
            {
                serverThread.join();
            }
        }

        static std::thread serverThread;
    };

    std::thread EopLoadTest::serverThread;

    /**
     * @brief Test with concurrent clients sending multiple requests each (Reduced for CI).
     */
    TEST_F(EopLoadTest, HighLoadConcurrentClientsCI)
    {
        std::atomic<int> readyCount {0};
        std::atomic<bool> startFlag {false};
        std::atomic<int> errorCount {0};
        std::atomic<int> completedCount {0};

        std::vector<std::thread> clients;
        clients.reserve(kNumClients);

        std::cout << "[ INFO ] Spawning " << kNumClients << " clients..." << std::endl;

        for (int i = 0; i < kNumClients; ++i)
        {
            clients.emplace_back(
                [&, i]()
                {
                    readyCount++;
                    while (!startFlag.load())
                    {
                        std::this_thread::yield();
                    }

                    bool ok = true;

                    int sock = socket(AF_INET, SOCK_STREAM, 0);
                    if (sock < 0)
                    {
                        errorCount++;
                        return;
                    }

                    // Use a larger timeout to avoid false negatives under heavy load
                    struct timeval tv;
                    tv.tv_sec = 30;
                    tv.tv_usec = 0;
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
                    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

                    struct sockaddr_in addr = {};
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(static_cast<uint16_t>(kLoadTestPort));
                    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

                    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
                    {
                        errorCount++;
                        close(sock);
                        return;
                    }

                    // First register the client
                    std::string nodeId = "load-client-" + std::to_string(i);
                    std::string regMsg =
                        "REGISTER node_id=" + nodeId + " bunker_name=LoadTest ip_address=127.0.0.1 capacity=100\n";
                    if (send(sock, regMsg.c_str(), regMsg.size(), 0) < 0)
                    {
                        errorCount++;
                        close(sock);
                        return;
                    }

                    char buf[1024];
                    ssize_t r = recv(sock, buf, sizeof(buf), 0);
                    if (r <= 0)
                    {
                        errorCount++;
                        close(sock);
                        return;
                    }

                    // Send 10,000 heartbeats
                    for (int j = 0; j < kRequestsPerClient; ++j)
                    {
                        static const std::string hbMsg = "HEARTBEAT\n";
                        if (send(sock, hbMsg.c_str(), hbMsg.size(), 0) < 0)
                        {
                            ok = false;
                            errorCount++;
                            break;
                        }

                        r = recv(sock, buf, sizeof(buf), 0);
                        if (r <= 0)
                        {
                            ok = false;
                            errorCount++;
                            break;
                        }
                    }

                    close(sock);
                    if (ok)
                    {
                        completedCount++;
                    }
                });
        }

        // Wait for all threads to be ready
        while (readyCount.load() < kNumClients)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::cout << "[ INFO ] All clients ready. Starting load test..." << std::endl;
        auto startTime = std::chrono::steady_clock::now();
        startFlag = true;

        // Join all threads
        for (auto& t : clients)
        {
            t.join();
        }

        auto endTime = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

        std::cout << "[ INFO ] Load test finished." << std::endl;
        std::cout << "[ INFO ] Total time: " << duration << " ms" << std::endl;
        std::cout << "[ INFO ] Error count: " << errorCount.load() << std::endl;
        std::cout << "[ INFO ] Completed clients: " << completedCount.load() << std::endl;

        EXPECT_EQ(errorCount.load(), 0) << "There were errors during the load test.";
        EXPECT_EQ(completedCount.load(), kNumClients) << "Not all clients completed successfully.";

        if (duration > 0)
        {
            double totalRequests = static_cast<double>(completedCount.load()) * kRequestsPerClient;
            double rps = totalRequests / (duration / 1000.0);
            std::cout << "[ INFO ] Throughput: " << rps << " requests/second" << std::endl;
        }
    }
} // namespace