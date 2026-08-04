/**
 * @file main.cpp
 * @brief HPC Engine entry point.
 *
 * Default mode:
 *   Reads one JSON request from stdin, processes it, writes one response to stdout.
 *
 * Socket mode:
 *   Connects to the EOP server, registers as an HPC Engine node, and processes
 *   ANALYZE_GRAPH requests over the same persistent TCP socket.
 *
 * Exit codes:
 *   0 — Success
 *   1 — Validation, parse, or socket error
 *   2 — Unexpected internal error
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "hpc/engine.hpp"

namespace
{

    constexpr const char* HEARTBEAT_FILE_PATH = "/tmp/hpc-engine-heartbeat";
    constexpr int HEARTBEAT_INTERVAL_SECONDS = 5;

    struct SocketModeConfig
    {
        std::string serverHost = "127.0.0.1";
        int serverPort = 9090;
        std::string nodeId = "hpc-engine";
        std::string bunkerName = "HPC";
        std::string ipAddress = "127.0.0.1";
        int capacity = 1;
    };

    struct RetryConfig
    {
        int maxAttempts = 5;
        int baseDelayMs = 250;
        int maxDelayMs = 5000;
    };

    constexpr double RETRY_BACKOFF_MULTIPLIER = 2.0;
    constexpr double RETRY_JITTER_FRACTION = 0.2;

    void logRetryEvent(const std::string& event, const SocketModeConfig& config, int attempt, int maxAttempts,
                        int backoffMs, const std::string& error)
    {
        nlohmann::json entry;
        entry["level"] = "info";
        entry["event"] = event;
        entry["attempt"] = attempt;
        entry["max_attempts"] = maxAttempts;
        entry["node_id"] = config.nodeId;
        entry["backoff_ms"] = backoffMs;
        entry["error"] = error;

        std::cerr << entry.dump() << '\n';
    }

    bool hasSocketModeArgs(int argc, char* argv[])
    {
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--server-host" || std::string(argv[i]) == "--server-port" ||
                std::string(argv[i]) == "--node-id")
            {
                return true;
            }
        }

        return false;
    }

    bool parseSocketModeArgs(int argc, char* argv[], SocketModeConfig& config, RetryConfig& retryConfig)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];

            auto requireValue = [&](const std::string& option) -> const char*
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "Missing value for " << option << '\n';
                    return nullptr;
                }

                ++i;
                return argv[i];
            };

            if (arg == "--server-host")
            {
                const char* value = requireValue(arg);
                if (value == nullptr)
                {
                    return false;
                }
                config.serverHost = value;
            }
            else if (arg == "--server-port")
            {
                const char* value = requireValue(arg);
                if (value == nullptr)
                {
                    return false;
                }

                try
                {
                    config.serverPort = std::stoi(value);
                }
                catch (...)
                {
                    std::cerr << "Invalid value for --server-port\n";
                    return false;
                }
            }
            else if (arg == "--node-id")
            {
                const char* value = requireValue(arg);
                if (value == nullptr)
                {
                    return false;
                }
                config.nodeId = value;
            }
            else if (arg == "--bunker-name")
            {
                const char* value = requireValue(arg);
                if (value == nullptr)
                {
                    return false;
                }
                config.bunkerName = value;
            }
            else if (arg == "--ip-address")
            {
                const char* value = requireValue(arg);
                if (value == nullptr)
                {
                    return false;
                }
                config.ipAddress = value;
            }
            else if (arg == "--capacity")
            {
                const char* value = requireValue(arg);
                if (value == nullptr)
                {
                    return false;
                }

                try
                {
                    config.capacity = std::stoi(value);
                }
                catch (...)
                {
                    std::cerr << "Invalid value for --capacity\n";
                    return false;
                }
            }
            else if (arg == "--retry-max-attempts")
            {
                const char* value = requireValue(arg);
                if (value == nullptr)
                {
                    return false;
                }

                try
                {
                    retryConfig.maxAttempts = std::stoi(value);
                }
                catch (...)
                {
                    std::cerr << "Invalid value for --retry-max-attempts\n";
                    return false;
                }
            }
            else if (arg == "--retry-base-delay-ms")
            {
                const char* value = requireValue(arg);
                if (value == nullptr)
                {
                    return false;
                }

                try
                {
                    retryConfig.baseDelayMs = std::stoi(value);
                }
                catch (...)
                {
                    std::cerr << "Invalid value for --retry-base-delay-ms\n";
                    return false;
                }
            }
            else
            {
                std::cerr << "Unknown argument: " << arg << '\n';
                return false;
            }
        }

        if (config.serverPort <= 0 || config.serverPort > 65535)
        {
            std::cerr << "Invalid server port range\n";
            return false;
        }

        if (config.nodeId.empty())
        {
            std::cerr << "node-id cannot be empty\n";
            return false;
        }

        if (config.capacity <= 0)
        {
            std::cerr << "capacity must be positive\n";
            return false;
        }

        if (retryConfig.maxAttempts <= 0)
        {
            std::cerr << "retry-max-attempts must be positive\n";
            return false;
        }

        if (retryConfig.baseDelayMs <= 0)
        {
            std::cerr << "retry-base-delay-ms must be positive\n";
            return false;
        }

        return true;
    }

    bool sendAll(int fd, const std::string& payload)
    {
        std::size_t totalSent = 0;

        while (totalSent < payload.size())
        {
            const ssize_t sent = send(fd, payload.data() + totalSent, payload.size() - totalSent, 0);

            if (sent < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                return false;
            }

            if (sent == 0)
            {
                return false;
            }

            totalSent += static_cast<std::size_t>(sent);
        }

        return true;
    }

    bool readLine(int fd, std::string& line)
    {
        line.clear();

        char ch = '\0';
        while (true)
        {
            const ssize_t received = recv(fd, &ch, 1, 0);

            if (received == 0)
            {
                return !line.empty();
            }

            if (received < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                return false;
            }

            if (ch == '\n')
            {
                return true;
            }

            if (ch != '\r')
            {
                line.push_back(ch);
            }
        }
    }

    constexpr int SOCKET_TIMEOUT_SECONDS = 3;

    int connectToServer(const SocketModeConfig& config)
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            std::cerr << "Failed to create socket\n";
            return -1;
        }

        struct timeval socketTimeout = {};
        socketTimeout.tv_sec = SOCKET_TIMEOUT_SECONDS;
        socketTimeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &socketTimeout, sizeof(socketTimeout));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &socketTimeout, sizeof(socketTimeout));

        struct sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(static_cast<uint16_t>(config.serverPort));

        if (inet_pton(AF_INET, config.serverHost.c_str(), &serverAddr.sin_addr) != 1)
        {
            std::cerr << "Invalid server host: " << config.serverHost << '\n';
            close(sock);
            return -1;
        }

        if (connect(sock, reinterpret_cast<struct sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0)
        {
            std::cerr << "Failed to connect to EOP server\n";
            close(sock);
            return -1;
        }

        return sock;
    }

    /**
     * @brief Clears the connect/register-phase recv/send timeouts.
     *
     * The steady-state request loop must block indefinitely waiting for the
     * next ANALYZE_GRAPH request — the bounded timeout from connectToServer()
     * is a Tier 1 (handshake-only) safeguard and must not fire while idle.
     */
    void clearSocketTimeouts(int sock)
    {
        struct timeval noTimeout = {};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &noTimeout, sizeof(noTimeout));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &noTimeout, sizeof(noTimeout));
    }

    std::string buildRegisterCommand(const SocketModeConfig& config)
    {
        std::ostringstream command;
        command << "REGISTER node_id=" << config.nodeId << " bunker_name=" << config.bunkerName
                << " ip_address=" << config.ipAddress << " capacity=" << config.capacity << '\n';
        return command.str();
    }

    int computeBackoffMs(int attempt, const RetryConfig& retryConfig)
    {
        const double raw = static_cast<double>(retryConfig.baseDelayMs) *
                            std::pow(RETRY_BACKOFF_MULTIPLIER, static_cast<double>(attempt - 1));
        const double capped = std::min(raw, static_cast<double>(retryConfig.maxDelayMs));

        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<double> jitter(1.0 - RETRY_JITTER_FRACTION, 1.0 + RETRY_JITTER_FRACTION);

        return static_cast<int>(capped * jitter(rng));
    }

    /**
     * @brief Connects and registers with bounded retry/backoff (Tier 1 resilience).
     *
     * Only guards the initial handshake, not the steady-state loop: a mid-loop
     * disconnect after a successful registration is left to Kubernetes'
     * restartPolicy (re-entering main(), which re-runs this Tier 1 loop).
     * @return Connected, registered socket fd, or -1 once maxAttempts is exhausted.
     */
    int establishSession(const SocketModeConfig& config, const RetryConfig& retryConfig)
    {
        for (int attempt = 1; attempt <= retryConfig.maxAttempts; ++attempt)
        {
            int sock = connectToServer(config);
            if (sock < 0)
            {
                if (attempt == retryConfig.maxAttempts)
                {
                    break;
                }

                const int backoffMs = computeBackoffMs(attempt, retryConfig);
                logRetryEvent("connect_attempt_failed", config, attempt, retryConfig.maxAttempts, backoffMs,
                              "connect failed");
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                continue;
            }

            const std::string registerCommand = buildRegisterCommand(config);
            if (!sendAll(sock, registerCommand))
            {
                close(sock);
                if (attempt == retryConfig.maxAttempts)
                {
                    break;
                }

                const int backoffMs = computeBackoffMs(attempt, retryConfig);
                logRetryEvent("register_failed", config, attempt, retryConfig.maxAttempts, backoffMs,
                              "failed to send REGISTER");
                std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
                continue;
            }

            bool registered = false;
            std::string registerResponse;
            for (int responseAttempts = 0; responseAttempts < 3; ++responseAttempts)
            {
                if (!readLine(sock, registerResponse))
                {
                    break;
                }

                if (registerResponse.find("ACK") != std::string::npos)
                {
                    registered = true;
                    break;
                }

                if (registerResponse.find("ERROR") != std::string::npos)
                {
                    break;
                }
            }

            if (registered)
            {
                logRetryEvent("registered", config, attempt, retryConfig.maxAttempts, 0, "");
                return sock;
            }

            close(sock);
            if (attempt == retryConfig.maxAttempts)
            {
                break;
            }

            const int backoffMs = computeBackoffMs(attempt, retryConfig);
            logRetryEvent("register_failed", config, attempt, retryConfig.maxAttempts, backoffMs,
                          "registration rejected or timed out: " + registerResponse);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
        }

        logRetryEvent("connect_exhausted", config, retryConfig.maxAttempts, retryConfig.maxAttempts, 0,
                      "exhausted retry attempts");
        return -1;
    }

    void writeHeartbeat()
    {
        std::ofstream heartbeatFile(HEARTBEAT_FILE_PATH, std::ios::trunc);
        if (heartbeatFile)
        {
            heartbeatFile << static_cast<long long>(std::time(nullptr));
        }
    }

    void runHeartbeatLoop(std::atomic<bool>& running)
    {
        while (running.load())
        {
            writeHeartbeat();
            std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL_SECONDS));
        }
    }

    int runBatchMode()
    {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();

        const hpc::EngineProcessResult result = hpc::processAnalyzeGraphRequest(buffer.str());

        std::cout << result.payload;
        return result.exit_code;
    }

    int runSocketMode(const SocketModeConfig& config, const RetryConfig& retryConfig)
    {
        int sock = establishSession(config, retryConfig);
        if (sock < 0)
        {
            std::cerr << "HPC Engine failed to establish a session after " << retryConfig.maxAttempts
                       << " attempts\n";
            return 1;
        }

        std::cerr << "HPC Engine registered as node_id=" << config.nodeId << '\n';

        clearSocketTimeouts(sock);

        std::atomic<bool> heartbeatRunning{true};
        std::thread heartbeatThread(runHeartbeatLoop, std::ref(heartbeatRunning));

        auto stopHeartbeat = [&]()
        {
            heartbeatRunning.store(false);
            if (heartbeatThread.joinable())
            {
                heartbeatThread.join();
            }
        };

        std::string request;
        while (readLine(sock, request))
        {
            if (request.empty())
            {
                continue;
            }

            const hpc::EngineProcessResult result = hpc::processAnalyzeGraphRequest(request);
            if (!sendAll(sock, result.payload))
            {
                std::cerr << "Failed to send HPC Engine response\n";
                stopHeartbeat();
                close(sock);
                return 1;
            }
        }

        stopHeartbeat();
        close(sock);
        return 0;
    }

} // namespace

int main(int argc, char* argv[])
{
    try
    {
        if (!hasSocketModeArgs(argc, argv))
        {
            return runBatchMode();
        }

        SocketModeConfig config;
        RetryConfig retryConfig;
        if (!parseSocketModeArgs(argc, argv, config, retryConfig))
        {
            return 1;
        }

        return runSocketMode(config, retryConfig);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Unexpected HPC Engine error: " << e.what() << '\n';
        return 2;
    }
}