#include "readinessPort.hpp"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <utility>

namespace
{
    constexpr int LISTEN_BACKLOG = 16;

    // Owns a socket fd: closed when it goes out of scope or is reset.
    class SocketFd
    {
    public:
        SocketFd() = default;
        ~SocketFd()
        {
            reset();
        }
        SocketFd(const SocketFd&) = delete;
        SocketFd& operator=(const SocketFd&) = delete;

        int get() const
        {
            return m_fd;
        }
        bool valid() const
        {
            return m_fd >= 0;
        }
        void reset(int fd = -1)
        {
            if (m_fd >= 0)
            {
                close(m_fd);
            }
            m_fd = fd;
        }

    private:
        int m_fd = -1;
    };

    int openListener(int port)
    {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return -1;
        }
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(fd, LISTEN_BACKLOG) < 0)
        {
            close(fd);
            return -1;
        }
        return fd;
    }
} // namespace

ReadinessPort::ReadinessPort(int port, std::function<bool()> ready, int checkMs)
    : m_port(port)
    , m_ready(std::move(ready))
    , m_checkMs(checkMs)
{
}

ReadinessPort::~ReadinessPort()
{
    stop();
}

void ReadinessPort::start()
{
    if (m_running.exchange(true))
    {
        return;
    }
    m_thread = std::thread(&ReadinessPort::run, this);
}

void ReadinessPort::stop()
{
    m_running = false;
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

bool ReadinessPort::listening() const
{
    return m_listening.load();
}

void ReadinessPort::run()
{
    SocketFd listener;

    while (m_running.load())
    {
        const bool ready = m_ready();
        if (ready && !listener.valid())
        {
            listener.reset(openListener(m_port));
        }
        else if (!ready && listener.valid())
        {
            listener.reset();
        }
        m_listening = listener.valid();

        if (!listener.valid())
        {
            poll(nullptr, 0, m_checkMs);
            continue;
        }
        pollfd pfd {listener.get(), POLLIN, 0};
        if (poll(&pfd, 1, m_checkMs) > 0 && (pfd.revents & POLLIN) != 0)
        {
            // The probe only needs the handshake: accept and close.
            SocketFd probe;
            probe.reset(accept(listener.get(), nullptr, nullptr));
        }
    }
    m_listening = false;
}
