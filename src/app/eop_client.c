#include "../../include/eop_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

__thread char g_last_error[256] = {0};

static void set_last_error(const char* msg)
{
    strncpy(g_last_error, msg, sizeof(g_last_error) - 1);
    g_last_error[sizeof(g_last_error) - 1] = '\0';
}

const char* eop_last_error(void)
{
    return g_last_error;
}

struct eop_client
{
    int fd;
    pthread_mutex_t lock;
};

static int set_nonblocking(int fd, int nonblocking)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    if (nonblocking)
    {
        flags |= O_NONBLOCK;
    }
    else
    {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags);
}

eop_client_t* eop_connect(const char* host, uint16_t port, uint32_t timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        set_last_error("Failed to create socket");
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0)
    {
        close(fd);
        set_last_error("Invalid host address");
        return NULL;
    }

    if (set_nonblocking(fd, 1) < 0)
    {
        close(fd);
        set_last_error("Failed to set socket non-blocking");
        return NULL;
    }

    int res = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (res < 0 && errno != EINPROGRESS)
    {
        close(fd);
        set_last_error("Connection failed");
        return NULL;
    }

    if (res < 0)
    {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        res = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (res <= 0)
        {
            close(fd);
            set_last_error(res == 0 ? "Connection timeout" : "select() failed");
            return NULL;
        }

        int so_error;
        socklen_t len = sizeof(so_error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error != 0)
        {
            close(fd);
            set_last_error("Connection refused or network unreachable");
            return NULL;
        }
    }

    if (set_nonblocking(fd, 0) < 0)
    {
        close(fd);
        set_last_error("Failed to restore socket blocking mode");
        return NULL;
    }

    eop_client_t* client = (eop_client_t*)malloc(sizeof(struct eop_client));
    if (!client)
    {
        close(fd);
        set_last_error("Memory allocation failed");
        return NULL;
    }

    client->fd = fd;
    pthread_mutex_init(&client->lock, NULL);

    return client;
}

void eop_disconnect(eop_client_t* client)
{
    if (!client)
        return;
    pthread_mutex_lock(&client->lock);
    if (client->fd >= 0)
    {
        close(client->fd);
        client->fd = -1;
    }
    pthread_mutex_unlock(&client->lock);
    pthread_mutex_destroy(&client->lock);
    free(client);
}

eop_response_t*
eop_send_command(eop_client_t* client, uint8_t cmd_type, const uint8_t* payload, size_t len, uint32_t timeout_ms)
{
    if (!client)
    {
        set_last_error("Invalid client handler");
        return NULL;
    }

    eop_response_t* response = (eop_response_t*)malloc(sizeof(eop_response_t));
    if (!response)
    {
        set_last_error("Memory allocation failed");
        return NULL;
    }
    response->status_code = EOP_OK;
    response->payload = NULL;
    response->payload_len = 0;

    pthread_mutex_lock(&client->lock);

    if (client->fd < 0)
    {
        pthread_mutex_unlock(&client->lock);
        response->status_code = EOP_ERR_DISCONNECTED;
        set_last_error("Client disconnected");
        return response;
    }

    char buffer[2048] = {0};
    size_t to_send = 0;

    if (cmd_type == 255)
    {
        if (len >= sizeof(buffer))
            len = sizeof(buffer) - 1;
        memcpy(buffer, payload, len);
        to_send = len;
    }
    else
    {
        const char* cmd_str = "";
        switch (cmd_type)
        {
            case 0: cmd_str = "REGISTER "; break;
            case 3: cmd_str = "QUERY_NODE "; break;
            case 5: cmd_str = "LIST_NODES"; break;
            case 6: cmd_str = "HEARTBEAT"; break;
            default: cmd_str = ""; break;
        }
        if (payload && len > 0)
        {
            to_send = snprintf(buffer, sizeof(buffer), "%s%.*s", cmd_str, (int)len, (const char*)payload);
        }
        else
        {
            to_send = snprintf(buffer, sizeof(buffer), "%s", cmd_str);
        }
    }

    set_nonblocking(client->fd, 1);

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(client->fd, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int res = select(client->fd + 1, NULL, &wfds, NULL, &tv);
    if (res <= 0)
    {
        set_nonblocking(client->fd, 0);
        pthread_mutex_unlock(&client->lock);
        response->status_code = (res == 0) ? EOP_ERR_TIMEOUT : EOP_ERR_DISCONNECTED;
        set_last_error(res == 0 ? "Send timeout" : "Send select failed");
        return response;
    }

    ssize_t sent = send(client->fd, buffer, to_send, 0);
    if (sent <= 0)
    {
        set_nonblocking(client->fd, 0);
        pthread_mutex_unlock(&client->lock);
        response->status_code = EOP_ERR_DISCONNECTED;
        set_last_error("Disconnected during send");
        return response;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(client->fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    res = select(client->fd + 1, &rfds, NULL, NULL, &tv);
    if (res <= 0)
    {
        set_nonblocking(client->fd, 0);
        pthread_mutex_unlock(&client->lock);
        response->status_code = (res == 0) ? EOP_ERR_TIMEOUT : EOP_ERR_DISCONNECTED;
        set_last_error(res == 0 ? "Receive timeout" : "Receive select failed");
        return response;
    }

    char recv_buf[2048] = {0};
    ssize_t received = recv(client->fd, recv_buf, sizeof(recv_buf) - 1, 0);
    set_nonblocking(client->fd, 0);

    pthread_mutex_unlock(&client->lock);

    if (received <= 0)
    {
        response->status_code = EOP_ERR_DISCONNECTED;
        set_last_error("Disconnected during receive");
        return response;
    }

    response->payload_len = received;
    response->payload = (uint8_t*)malloc(received + 1);
    if (response->payload)
    {
        memcpy(response->payload, recv_buf, received);
        response->payload[received] = '\0';
    }

    return response;
}

void eop_response_free(eop_response_t* response)
{
    if (!response)
        return;
    if (response->payload)
    {
        free(response->payload);
    }
    free(response);
}
