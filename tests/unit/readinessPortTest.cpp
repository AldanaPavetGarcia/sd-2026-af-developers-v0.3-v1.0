#include "../../include/readinessPort.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace
{
    constexpr int READY_PORT = 39927;
    constexpr int CHECK_MS = 20;
    constexpr int SETTLE_MS = 200;

    bool canConnect(int port)
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        const bool ok = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        close(fd);
        return ok;
    }

    void settle()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(SETTLE_MS));
    }
} // namespace

// The Kubernetes tcpSocket probe passes exactly while the predicate holds (ADR-027 §3).
TEST(ReadinessPortTest, ListensOnlyWhileReady)
{
    std::atomic<bool> ready {true};
    ReadinessPort port(READY_PORT, [&ready]() { return ready.load(); }, CHECK_MS);

    EXPECT_FALSE(canConnect(READY_PORT)) << "nothing listens before start()";
    port.start();
    settle();
    EXPECT_TRUE(port.listening());
    EXPECT_TRUE(canConnect(READY_PORT));
    EXPECT_TRUE(canConnect(READY_PORT)) << "every probe is accepted";

    ready = false;
    settle();
    EXPECT_FALSE(port.listening());
    EXPECT_FALSE(canConnect(READY_PORT)) << "not ready: the probe fails";

    ready = true;
    settle();
    EXPECT_TRUE(canConnect(READY_PORT)) << "ready again";

    port.stop();
    EXPECT_FALSE(port.listening());
    EXPECT_FALSE(canConnect(READY_PORT));
    port.stop(); // idempotent
}

TEST(ReadinessPortTest, AStartAfterStartIsHarmless)
{
    ReadinessPort port(READY_PORT, []() { return true; }, CHECK_MS);
    port.start();
    port.start();
    settle();
    EXPECT_TRUE(canConnect(READY_PORT));
}

TEST(ReadinessPortTest, APortInUseIsRetriedNotFatal)
{
    // Something else holds the port: the readiness port keeps trying and stays not listening.
    const int blocker = socket(AF_INET, SOCK_STREAM, 0);
    // Earlier probes may have left the port in TIME_WAIT.
    int reuse = 1;
    setsockopt(blocker, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(READY_PORT));
    addr.sin_addr.s_addr = INADDR_ANY;
    ASSERT_EQ(bind(blocker, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(blocker, 1), 0);

    ReadinessPort port(READY_PORT, []() { return true; }, CHECK_MS);
    port.start();
    settle();
    EXPECT_FALSE(port.listening());
    close(blocker);
    settle();
    EXPECT_TRUE(port.listening()) << "picked up once the port is free";
}
