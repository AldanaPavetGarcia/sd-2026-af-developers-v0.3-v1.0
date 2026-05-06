#include "eop_client.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("Connecting to EOP Server...\n");
    
    eop_client_t* client = eop_connect("127.0.0.1", 9098, 5000);
    if (!client) {
        fprintf(stderr, "Failed to connect: %s\n", eop_last_error());
        return 1;
    }

    printf("Connected successfully!\n");

    const char* payload = "node_id=basic-1 bunker_name=Basic ip_address=10.0.0.99 capacity=5";
    printf("Sending REGISTER command...\n");
    
    eop_response_t* resp = eop_send_command(client, 0, (const uint8_t*)payload, strlen(payload), 5000);
    
    if (resp->status_code == EOP_OK) {
        printf("Received response: %s\n", resp->payload);
    } else {
        fprintf(stderr, "Command failed (status %d): %s\n", resp->status_code, eop_last_error());
    }

    eop_response_free(resp);
    eop_disconnect(client);

    return 0;
}
