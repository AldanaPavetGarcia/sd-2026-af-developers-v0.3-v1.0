#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "../../include/eop_client.h"

namespace {
    const int kClientTestPort = 9050 + (getpid() % 1000);

    // Mock server for testing client behaviors
    std::atomic<bool> stopMockServer{false};
    std::atomic<int> mockServerAction{0}; // 0 = echo, 1 = drop immediately, 2 = delay, 3 = drop mid-wait

    void mockServerLoop() {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(kClientTestPort);

        bind(server_fd, (struct sockaddr *)&address, sizeof(address));
        listen(server_fd, 5);

        // Non-blocking accept
        fcntl(server_fd, F_SETFL, O_NONBLOCK);

        while (!stopMockServer) {
            int new_socket = accept(server_fd, nullptr, nullptr);
            if (new_socket >= 0) {
                int action = mockServerAction.load();
                
                if (action == 1) { // Drop immediately
                    close(new_socket);
                } else {
                    char buffer[1024] = {0};
                    ssize_t valread = recv(new_socket, buffer, 1024, 0);
                    
                    if (valread > 0) {
                        if (action == 0) { // Echo success
                            std::string response = "ACK " + std::string(buffer);
                            send(new_socket, response.c_str(), response.size(), 0);
                        } else if (action == 2) { // Delay response
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            if (!stopMockServer) {
                                std::string response = "ACK DELAYED";
                                send(new_socket, response.c_str(), response.size(), 0);
                            }
                        } else if (action == 3) { // Drop mid-wait
                            close(new_socket);
                            continue;
                        }
                    }
                    if (action != 3) {
                        close(new_socket);
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        close(server_fd);
    }
}

class ClientTest : public ::testing::Test {
protected:
    static std::thread* serverThread;

    static void SetUpTestSuite() {
        stopMockServer = false;
        mockServerAction = 0;
        serverThread = new std::thread(mockServerLoop);
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // wait for bind
    }

    static void TearDownTestSuite() {
        stopMockServer = true;
        if (serverThread->joinable()) {
            serverThread->join();
        }
        delete serverThread;
    }

    void SetUp() override {
        mockServerAction = 0;
    }
};

std::thread* ClientTest::serverThread = nullptr;

TEST_F(ClientTest, ConnectUnreachableHostFailsCleanly) {
    // 192.0.2.0/24 is TEST-NET-1 (unroutable)
    eop_client_t* client = eop_connect("192.0.2.1", 12345, 100);
    EXPECT_EQ(client, nullptr);
    EXPECT_NE(eop_last_error(), nullptr);
    std::string err = eop_last_error();
    EXPECT_FALSE(err.empty());
}

TEST_F(ClientTest, SendCommandNullHandler) {
    eop_response_t* resp = eop_send_command(nullptr, 0, (const uint8_t*)"test", 4, 100);
    EXPECT_EQ(resp, nullptr);
    EXPECT_NE(eop_last_error(), nullptr);
    
    // Safety check for free
    eop_response_free(nullptr); 
}

TEST_F(ClientTest, SuccessfulCommandResponse) {
    mockServerAction = 0;
    eop_client_t* client = eop_connect("127.0.0.1", kClientTestPort, 1000);
    ASSERT_NE(client, nullptr);

    const char* req = "node_id=foo";
    eop_response_t* resp = eop_send_command(client, 0, (const uint8_t*)req, strlen(req), 1000);
    
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->status_code, EOP_OK);
    EXPECT_NE(resp->payload, nullptr);
    EXPECT_GT(resp->payload_len, 0);

    eop_response_free(resp);
    eop_disconnect(client);
}

TEST_F(ClientTest, ServerDisconnectsMidWait) {
    mockServerAction = 3; // Drop mid-wait
    eop_client_t* client = eop_connect("127.0.0.1", kClientTestPort, 1000);
    ASSERT_NE(client, nullptr);

    const char* req = "node_id=foo";
    eop_response_t* resp = eop_send_command(client, 0, (const uint8_t*)req, strlen(req), 1000);
    
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->status_code, EOP_ERR_DISCONNECTED);

    eop_response_free(resp);
    eop_disconnect(client);
}

TEST_F(ClientTest, CommandTimeout) {
    mockServerAction = 2; // Delay response by 500ms
    eop_client_t* client = eop_connect("127.0.0.1", kClientTestPort, 1000);
    ASSERT_NE(client, nullptr);

    const char* req = "node_id=foo";
    // Set timeout to 100ms
    eop_response_t* resp = eop_send_command(client, 0, (const uint8_t*)req, strlen(req), 100);
    
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->status_code, EOP_ERR_TIMEOUT);

    eop_response_free(resp);
    eop_disconnect(client);
}

TEST_F(ClientTest, SequentialLeakCheck) {
    mockServerAction = 0;
    eop_client_t* client = eop_connect("127.0.0.1", kClientTestPort, 1000);
    ASSERT_NE(client, nullptr);

    const char* req = "node_id=cycle";
    for (int i = 0; i < 1000; ++i) {
        eop_response_t* resp = eop_send_command(client, 0, (const uint8_t*)req, strlen(req), 1000);
        ASSERT_NE(resp, nullptr);
        eop_response_free(resp);
        
        // Reconnect for each cycle since our dummy server closes connection after 1 msg
        eop_disconnect(client);
        client = eop_connect("127.0.0.1", kClientTestPort, 1000);
        ASSERT_NE(client, nullptr);
    }
    
    eop_disconnect(client);
}
