/**
 * @file testLoad.cpp
 * @brief High load test for EOP server.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "../../include/eopServer.hpp"

namespace
{
    // Use a unique port for this test to avoid conflicts
    const int kLoadTestPort = 11000 + (getpid() % 5000);

    // CI runs a reduced load (ADR-015); the original US-102 AC8 figures -- 1000 clients x 10,000
    // requests -- are reproduced by setting EOP_LOAD_CLIENTS / EOP_LOAD_REQUESTS_PER_CLIENT.
    constexpr int CI_NUM_CLIENTS = 20;
    constexpr int CI_REQUESTS_PER_CLIENT = 50;

    // Each client holds one socket and the server another, plus headroom for the process itself.
    constexpr int FDS_PER_CLIENT = 2;
    constexpr rlim_t FD_HEADROOM = 256;
    constexpr int MAX_CLIENTS_HEADROOM = 200;

    // Latencies are sampled, not all kept: 10 million requests would otherwise need 80 MB just
    // for the percentiles.
    constexpr long MAX_LATENCY_SAMPLES = 200000;

    int positiveEnv(const char* name, int fallback)
    {
        const char* raw = std::getenv(name);
        if (raw == nullptr)
        {
            return fallback;
        }
        const int value = std::atoi(raw);
        return value > 0 ? value : fallback;
    }

    // 0 = each client sends its next request as soon as the previous reply arrives (saturation).
    // A positive value paces every client at the same fixed rate, which is what US-102 AC2's
    // "identical workload" comparison between 1 and 100 clients needs: at saturation, latency
    // grows with the queue, not with how the server treats each client.
    const int kRequestIntervalMs = positiveEnv("EOP_LOAD_REQUEST_INTERVAL_MS", 0);

    const int kNumClients = positiveEnv("EOP_LOAD_CLIENTS", CI_NUM_CLIENTS);
    const int kRequestsPerClient = positiveEnv("EOP_LOAD_REQUESTS_PER_CLIENT", CI_REQUESTS_PER_CLIENT);
    const rlim_t kRequiredFds = static_cast<rlim_t>(kNumClients) * FDS_PER_CLIENT + FD_HEADROOM;
    const int kMaxClients = kNumClients + MAX_CLIENTS_HEADROOM;

    double percentile(std::vector<double>& sorted, double fraction)
    {
        if (sorted.empty())
        {
            return 0.0;
        }
        const auto index = static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1));
        return sorted[index];
    }

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
            setenv("EOP_MAX_CLIENTS", std::to_string(kMaxClients).c_str(), 1);
            setenv("EOP_HEARTBEAT_INTERVAL", "30", 1);
            setenv("EOP_IDLE_TIMEOUT", "60", 1);
            serverContext.maxClients = kMaxClients;
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

        const long totalRequests = static_cast<long>(kNumClients) * kRequestsPerClient;
        const long sampleStride = std::max(1L, totalRequests / MAX_LATENCY_SAMPLES);
        std::mutex latenciesMutex;
        std::vector<double> latenciesMs;
        latenciesMs.reserve(static_cast<size_t>(std::min(totalRequests, MAX_LATENCY_SAMPLES) + kNumClients));
        std::atomic<int> errorResponses {0};

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

                    std::vector<double> localLatenciesMs;
                    for (int j = 0; j < kRequestsPerClient; ++j)
                    {
                        static const std::string hbMsg = "HEARTBEAT\n";
                        const auto sentAt = std::chrono::steady_clock::now();
                        if (send(sock, hbMsg.c_str(), hbMsg.size(), 0) < 0)
                        {
                            ok = false;
                            errorCount++;
                            break;
                        }

                        r = recv(sock, buf, sizeof(buf) - 1, 0);
                        if (r <= 0)
                        {
                            ok = false;
                            errorCount++;
                            break;
                        }
                        buf[r] = '\0';

                        // US-102 AC8 asks for zero server-side errors, not only for a reply: a
                        // server that answered every heartbeat with ERROR would otherwise pass.
                        if (std::strstr(buf, "ERROR") != nullptr)
                        {
                            ok = false;
                            errorResponses++;
                            break;
                        }

                        // Taken before any pacing sleep, so it is the request's own round trip.
                        const double latencyMs =
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sentAt)
                                .count();
                        if ((static_cast<long>(i) * kRequestsPerClient + j) % sampleStride == 0)
                        {
                            localLatenciesMs.push_back(latencyMs);
                        }

                        if (kRequestIntervalMs > 0)
                        {
                            std::this_thread::sleep_until(sentAt + std::chrono::milliseconds(kRequestIntervalMs));
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(latenciesMutex);
                        latenciesMs.insert(latenciesMs.end(), localLatenciesMs.begin(), localLatenciesMs.end());
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

        std::sort(latenciesMs.begin(), latenciesMs.end());

        // The in-process server logs to stdout from its own threads; building each line first and
        // writing it in one call keeps these results from being interleaved with its JSON logs.
        const auto report = [](const std::string& line)
        {
            std::cout << line + "\n" << std::flush;
        };
        std::ostringstream summary;
        summary << "[ LOAD ] clients=" << kNumClients << " requests_per_client=" << kRequestsPerClient
                << " interval_ms=" << kRequestIntervalMs << " duration_ms=" << duration
                << " completed_clients=" << completedCount.load() << " errors=" << errorCount.load()
                << " server_error_responses=" << errorResponses.load();
        report(summary.str());
        std::ostringstream latency;
        latency << "[ LOAD ] latency_ms samples=" << latenciesMs.size() << " p50=" << percentile(latenciesMs, 0.50)
                << " p95=" << percentile(latenciesMs, 0.95) << " p99=" << percentile(latenciesMs, 0.99)
                << " max=" << (latenciesMs.empty() ? 0.0 : latenciesMs.back());
        report(latency.str());
        if (duration > 0)
        {
            std::ostringstream throughput;
            throughput << "[ LOAD ] throughput_rps="
                       << static_cast<double>(completedCount.load()) * kRequestsPerClient / (duration / 1000.0);
            report(throughput.str());
        }

        // The P99 < 200 ms NFR is asserted only when requested: sanitizer builds in CI run this
        // same test many times slower, and a latency bound there would measure the sanitizer.
        if (const int maxP99Ms = positiveEnv("EOP_LOAD_MAX_P99_MS", 0); maxP99Ms > 0)
        {
            EXPECT_LE(percentile(latenciesMs, 0.99), maxP99Ms) << "P99 latency above " << maxP99Ms << " ms";
        }

        EXPECT_EQ(errorCount.load(), 0) << "There were errors during the load test.";
        EXPECT_EQ(errorResponses.load(), 0) << "The server answered with ERROR during the load test.";
        EXPECT_EQ(completedCount.load(), kNumClients) << "Not all clients completed successfully.";
    }
} // namespace