/**
 * @file readinessPort.hpp
 * @brief TCP port that is open only while the server can serve (ADR-027 §3).
 *
 * @details Kubernetes readiness is a plain `tcpSocket` probe on this port, so no HTTP stack and no
 * shell are needed in the distroless image. While `ready()` returns true the port listens and
 * accepts (and immediately closes) connections; while it returns false the listener is closed and
 * the probe fails. The server's `ready()` is "registry backend healthy and not shutting down": a
 * replica that loses Redis, or that received SIGTERM and is draining, leaves the Service.
 */

#ifndef READINESS_PORT_HPP
#define READINESS_PORT_HPP

#include <atomic>
#include <functional>
#include <thread>

/**
 * @brief Opens and closes a listener following a readiness predicate.
 *
 * @invariant The worker thread runs between start() and stop() (or destruction); the listener
 * socket is owned by that thread and closed when it ends.
 */
class ReadinessPort
{
public:
    /**
     * @brief Prepares the port; nothing listens until start().
     * @param port TCP port.
     * @param ready Readiness predicate, evaluated every `checkMs`.
     * @param checkMs How often the predicate is evaluated.
     */
    ReadinessPort(int port, std::function<bool()> ready, int checkMs = 200);

    /**
     * @brief Stops the worker thread and closes the listener.
     */
    ~ReadinessPort();

    ReadinessPort(const ReadinessPort&) = delete;
    ReadinessPort& operator=(const ReadinessPort&) = delete;

    /**
     * @brief Starts following the predicate.
     */
    void start();

    /**
     * @brief Stops following the predicate and closes the listener; idempotent.
     */
    void stop();

    /**
     * @brief Whether the listener is open right now.
     * @return true while listening.
     */
    bool listening() const;

private:
    void run();

    int m_port;                            ///< TCP port.
    std::function<bool()> m_ready;         ///< Readiness predicate.
    int m_checkMs;                         ///< Predicate period, also the accept poll timeout.
    std::atomic<bool> m_running {false};   ///< Worker thread keeps going while true.
    std::atomic<bool> m_listening {false}; ///< Listener open.
    std::thread m_thread;                  ///< Worker thread.
};

#endif // READINESS_PORT_HPP
