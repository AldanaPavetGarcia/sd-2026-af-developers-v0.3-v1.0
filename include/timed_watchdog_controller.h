#ifndef TIMED_WATCHDOG_CONTROLLER_H
#define TIMED_WATCHDOG_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../lib/include/commonProtocol.hpp"
#include "../lib/include/eventAuditLogger.hpp"

#ifdef __cplusplus
extern "C"
{
#endif

#define WATCHDOG_WELCOME_TIMEOUT_DEFAULT 500
#define WATCHDOG_IDLE_TIMEOUT_DEFAULT    30000
#define WATCHDOG_BUFFER_SIZE             1024
#define WATCHDOG_SESSION_REQ_BUFFER_SIZE 32
#define MS_TO_SEC                        1000
#define USEC_TO_MS                       1000

    /**
     * @brief Watchdog states similar to the example class.
     */
    typedef enum
    {
        WATCHDOG_INIT_STATUS,
        WATCHDOG_WAIT_WELCOME_STATUS,
        WATCHDOG_CONNECTED_STATUS,
        WATCHDOG_ERROR_STATUS
    } WatchdogStatus;

    typedef struct
    {
        int sock;
        WatchdogStatus status;
        struct timeval start_time;
        struct timeval last_action;
        int welcome_timeout_ms;
        int idle_timeout_ms;
        bool should_exit_process;
    } Watchdog;

    void watchdog_init(Watchdog* watchdog, int socket_fd);
    int watchdog_start(Watchdog* watchdog);
    void watchdog_stop(Watchdog* watchdog);

#ifdef __cplusplus
}
#endif

#endif
