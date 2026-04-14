#ifndef EOP_PROTOCOL_CLIENT_H
#define EOP_PROTOCOL_CLIENT_H

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @brief Header file for the client.
 */

void run_eop_client(const char* host, const char* port);

#endif
