#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../../include/eop_client.h"

namespace {

#ifdef EXODUS_ENABLE_ASAN_BUILD
constexpr int kSequentialDisconnectCycles = 1000;
#else
constexpr int kSequentialDisconnectCycles = 20;
#endif

constexpr int kClientTimeoutMs = 1000;
constexpr int kListenBacklog = 64;
constexpr int kAcceptSleepMs = 2;

class SequentialMockServer {
public:
    SequentialMockServer() = default;

    SequentialMockServer(const SequentialMockServer&) = delete;
    SequentialMockServer& operator=(const SequentialMockServer&) = delete;

    ~SequentialMockServer() {
        stop();
    }

    bool start() {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            return false;
        }

        int opt = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);

        if (bind(server_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        socklen_t address_len = sizeof(address);
        if (getsockname(server_fd_, reinterpret_cast<sockaddr*>(&address), &address_len) < 0) {
            close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        port_ = ntohs(address.sin_port);

        if (listen(server_fd_, kListenBacklog) < 0) {
            close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        int flags = fcntl(server_fd_, F_GETFL, 0);
        if (flags < 0 || fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(server_fd_);
            server_fd_ = -1;
            return false;
        }

        stop_requested_ = false;
        worker_ = std::thread(&SequentialMockServer::run, this);
        return true;
    }

    void stop() {
        stop_requested_ = true;

        if (worker_.joinable()) {
            worker_.join();
        }

        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
    }

    uint16_t port() const {
        return port_;
    }

private:
    void run() {
        while (!stop_requested_) {
            int client_fd = accept(server_fd_, nullptr, nullptr);
            if (client_fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kAcceptSleepMs));
                continue;
            }

            handleClient(client_fd);
        }
    }

    static void handleClient(int client_fd) {
        char buffer[1024] = {0};
        ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);

        if (received > 0) {
            const std::string response = "ACK " + std::string(buffer, static_cast<size_t>(received));
            send(client_fd, response.c_str(), response.size(), 0);
        }

        close(client_fd);
    }

    int server_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;
};

class ClientDisconnectSequentialTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(server_.start());
    }

    void TearDown() override {
        server_.stop();
    }

    SequentialMockServer server_;
};

}  // namespace

TEST(ClientDisconnectNullTest, DisconnectNullIsNoop) {
    eop_disconnect(nullptr);
}

TEST_F(ClientDisconnectSequentialTest, SequentialConnectSendDisconnectCycles) {
    const char* payload = "node_id=us107-sequential";

    for (int cycle = 0; cycle < kSequentialDisconnectCycles; ++cycle) {
        eop_client_t* client = eop_connect("127.0.0.1", server_.port(), kClientTimeoutMs);
        ASSERT_NE(client, nullptr) << "cycle=" << cycle << " error=" << eop_last_error();

        eop_response_t* response = eop_send_command(
            client,
            0,
            reinterpret_cast<const uint8_t*>(payload),
            std::strlen(payload),
            kClientTimeoutMs
        );

        if (response == nullptr) {
            eop_disconnect(client);
            FAIL() << "eop_send_command returned NULL at cycle=" << cycle
                   << " error=" << eop_last_error();
        }

        EXPECT_EQ(response->status_code, EOP_OK) << "cycle=" << cycle;
        EXPECT_NE(response->payload, nullptr) << "cycle=" << cycle;
        EXPECT_GT(response->payload_len, 0U) << "cycle=" << cycle;

        eop_response_free(response);
        eop_disconnect(client);
    }
}