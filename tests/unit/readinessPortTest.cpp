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
    constexpr int CHECK_MS = 20;
    constexpr int SETTLE_MS = 200;

    // A port nobody uses right now, chosen by the kernel: ctest runs these tests in parallel
    // processes, next to other tests' client sockets.
    int freePort()
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        close(fd);
        return ntohs(addr.sin_port);
    }

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
    const int readyPort = freePort();
    std::atomic<bool> ready {true};
    ReadinessPort port(readyPort, [&ready]() { return ready.load(); }, CHECK_MS);

    EXPECT_FALSE(canConnect(readyPort)) << "nothing listens before start()";
    port.start();
    settle();
    EXPECT_TRUE(port.listening());
    EXPECT_TRUE(canConnect(readyPort));
    EXPECT_TRUE(canConnect(readyPort)) << "every probe is accepted";

    ready = false;
    settle();
    EXPECT_FALSE(port.listening());
    EXPECT_FALSE(canConnect(readyPort)) << "not ready: the probe fails";

    ready = true;
    settle();
    EXPECT_TRUE(canConnect(readyPort)) << "ready again";

    port.stop();
    EXPECT_FALSE(port.listening());
    EXPECT_FALSE(canConnect(readyPort));
    port.stop(); // idempotent
}

TEST(ReadinessPortTest, AStartAfterStartIsHarmless)
{
    const int readyPort = freePort();
    ReadinessPort port(readyPort, []() { return true; }, CHECK_MS);
    port.start();
    port.start();
    settle();
    EXPECT_TRUE(canConnect(readyPort));
}

TEST(ReadinessPortTest, APortInUseIsRetriedNotFatal)
{
    // Something else holds the port: the readiness port keeps trying and stays not listening.
    // The kernel picks the blocker's port, so it cannot collide with another process.
    const int blocker = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    ASSERT_EQ(bind(blocker, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(blocker, 1), 0);
    socklen_t len = sizeof(addr);
    ASSERT_EQ(getsockname(blocker, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    const int readyPort = ntohs(addr.sin_port);

    ReadinessPort port(readyPort, []() { return true; }, CHECK_MS);
    port.start();
    settle();
    EXPECT_FALSE(port.listening());
    close(blocker);
    settle();
    EXPECT_TRUE(port.listening()) << "picked up once the port is free";
}
