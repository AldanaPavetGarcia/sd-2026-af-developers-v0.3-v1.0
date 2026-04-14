// NOLINTBEGIN(*)
#include "../include/timed_watchdog_controller.h"

static uint64_t watchdog_get_current_time_ms(void)
{
    struct timeval tv = {0, 0};
    gettimeofday(&tv, NULL);
    return (uint64_t)(tv.tv_sec) * MS_TO_SEC + (uint64_t)(tv.tv_usec) / USEC_TO_MS;
}

void watchdog_init(Watchdog* watchdog, int socket_fd)
{
    if (watchdog == NULL)
        return;
    watchdog->sock = socket_fd;
    watchdog->status = WATCHDOG_INIT_STATUS;
    watchdog->welcome_timeout_ms = WATCHDOG_WELCOME_TIMEOUT_DEFAULT;
    watchdog->idle_timeout_ms = WATCHDOG_IDLE_TIMEOUT_DEFAULT;
    watchdog->should_exit_process = false;
    srand(time(NULL));
}

int watchdog_start(Watchdog* watchdog)
{
    if (watchdog == NULL)
        return -1;

    char buffer[WATCHDOG_BUFFER_SIZE];
    uint64_t waiting_period_start_time = 0;

    while (!watchdog->should_exit_process)
    {
        switch (watchdog->status)
        {
            case WATCHDOG_INIT_STATUS:
            {
                audit_log_write("WATCHDOG: Starting time control...\n", INFO_LOG);
                gettimeofday(&watchdog->start_time, NULL);

                // Request random session number between 1 and MAX_CLIENTS
                int requested_session_id = (rand() % MAX_CLIENTS) + 1;
                char session_request_buffer[WATCHDOG_SESSION_REQ_BUFFER_SIZE];
                snprintf(
                    session_request_buffer, sizeof(session_request_buffer), "REQ_SESSION %d\n", requested_session_id);
                send(watchdog->sock, session_request_buffer, (int)strlen(session_request_buffer), 0);
                audit_log_write_fmt(CONNECTION_LOG, "WATCHDOG: Requesting session ID: %d\n", requested_session_id);

                watchdog->status = WATCHDOG_WAIT_WELCOME_STATUS;
                waiting_period_start_time = watchdog_get_current_time_ms();
                break;
            }

            case WATCHDOG_WAIT_WELCOME_STATUS:
            {
                fd_set readfds;
                struct timeval tv = {0, 0};

                uint64_t now = watchdog_get_current_time_ms();
                int elapsed = (int)(now - waiting_period_start_time);
                int remaining = watchdog->welcome_timeout_ms - elapsed;

                if (remaining <= 0)
                {
                    audit_log_write_fmt(
                        ERROR_LOG,
                        "WATCHDOG Error: Timeout of %d ms exceeded without receiving welcome message.\n",
                        watchdog->welcome_timeout_ms);
                    watchdog->status = WATCHDOG_ERROR_STATUS;
                    break;
                }

                tv.tv_sec = remaining / MS_TO_SEC;
                tv.tv_usec = (remaining % MS_TO_SEC) * USEC_TO_MS;

                FD_ZERO(&readfds);
                FD_SET(watchdog->sock, &readfds);

                int ret = select(watchdog->sock + 1, &readfds, NULL, NULL, &tv);
                if (ret > 0)
                {
                    ssize_t bytes = recv(watchdog->sock, buffer, sizeof(buffer) - 1, 0);
                    if (bytes > 0)
                    {
                        buffer[bytes] = '\0';
                        audit_log_write_fmt(CONNECTION_LOG, "WATCHDOG Welcome message received:\n%s\n", buffer);
                        watchdog->status = WATCHDOG_CONNECTED_STATUS;
                    }
                    else
                    {
                        audit_log_write("Error: Server closed connection before sending welcome.\n", ERROR_LOG);
                        watchdog->status = WATCHDOG_ERROR_STATUS;
                    }
                }
                else if (ret == 0)
                {
                    audit_log_write("WATCHDOG Error: Timeout exceeded (0 ms remaining).\n", ERROR_LOG);
                    watchdog->status = WATCHDOG_ERROR_STATUS;
                }
                else
                {
                    audit_log_write("WATCHDOG Network error in select()\n", ERROR_LOG);
                    watchdog->status = WATCHDOG_ERROR_STATUS;
                }
                break;
            }

            case WATCHDOG_CONNECTED_STATUS:
            {
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(watchdog->sock, &readfds);
                FD_SET(STDIN_FILENO, &readfds);

                struct timeval idle_tv = {0, 0};
                idle_tv.tv_sec = 1;
                idle_tv.tv_usec = 0;

                int n = select(watchdog->sock + 1, &readfds, NULL, NULL, &idle_tv);
                if (n > 0)
                {
                    if (FD_ISSET(watchdog->sock, &readfds))
                    {
                        ssize_t bytes = recv(watchdog->sock, buffer, sizeof(buffer) - 1, 0);
                        if (bytes <= 0)
                        {
                            audit_log_write("WATCHDOG: Connection closed by server.\n", CONNECTION_LOG);
                            watchdog->should_exit_process = true;
                        }
                        else
                        {
                            buffer[bytes] = '\0';
                            // Unexpected data handling could be added here
                        }
                    }
                    if (FD_ISSET(STDIN_FILENO, &readfds))
                    {
                        char input[MESSAGE_BUFFER_SIZE];
                        if (fgets(input, sizeof(input), stdin))
                        {
                            if (strncmp(input, "quit", 4) == 0 || strncmp(input, "exit", 4) == 0)
                            {
                                audit_log_write("WATCHDOG: Explicit closure by client.\n", CONNECTION_LOG);
                                watchdog->should_exit_process = true;
                            }
                        }
                    }
                }
                break;
            }

            case WATCHDOG_ERROR_STATUS: return -1;

            default:
                audit_log_write("WATCHDOG Undefined state\n", ERROR_LOG);
                watchdog->status = WATCHDOG_ERROR_STATUS;
                break;
        }
    }

    return 0;
}

void watchdog_stop(Watchdog* watchdog)
{
    if (watchdog != NULL)
    {
        watchdog->should_exit_process = true;
    }
}
// NOLINTEND(*)
