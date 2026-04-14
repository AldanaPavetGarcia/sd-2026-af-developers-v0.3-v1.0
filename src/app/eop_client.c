// NOLINTBEGIN(*)
#include "../include/eop_client.h"
#include "../include/timed_watchdog_controller.h"

void run_eop_client(const char* host, const char* port)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0 || res == NULL)
    {
        fprintf(stderr, "Error: Could not resolve host '%s'\n", host);
        return;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0)
    {
        fprintf(stderr, "Error: Could not create socket\n");
        freeaddrinfo(res);
        return;
    }

    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) < 0)
    {
        fprintf(stderr, "Error: Could not connect to %s:%s. Server may be down or inaccessible.\n", host, port);
        close(sock);
        freeaddrinfo(res);
        return;
    }

    freeaddrinfo(res);
    printf("Connection established with %s:%s.\n", host, port);
    printf("Type 'quit' or 'exit' to explicitly terminate the connection once connected.\n");

    fd_set readfds;
    char buffer[WATCHDOG_BUFFER_SIZE];

    while (1)
    {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(sock, &readfds);

        int max_fd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;
        int n = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (n < 0)
        {
            fprintf(stderr, "Select error\n");
            break;
        }

        if (FD_ISSET(sock, &readfds))
        {
            ssize_t bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (bytes <= 0)
            {
                printf("Server closed connection.\n");
                break;
            }
            buffer[bytes] = '\0';
            printf("%s", buffer);
            fflush(stdout);
        }

        if (FD_ISSET(STDIN_FILENO, &readfds))
        {
            if (fgets(buffer, sizeof(buffer), stdin))
            {
                if (strncmp(buffer, "quit", 4) == 0 || strncmp(buffer, "exit", 4) == 0)
                {
                    break;
                }
                send(sock, buffer, strlen(buffer), 0);
            }
            else
            {
                break; // EOF
            }
        }
    }

    close(sock);
    return;
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    run_eop_client(argv[1], argv[2]);
    return 0;
}
// NOLINTEND(*)
