/**
 * US-201 AC8 / US-203 AC2: "C client -> v0.1 server -> HPC engine -> result delivered to
 * client." The existing analyze_graph_real_engine.py smoke test proves the full server/engine
 * path is real, but drives it with a raw Python socket, not the actual C client library the
 * ACs name. This program is the missing "C client" leg: it links against libeop (eop_client.c)
 * and sends a real ANALYZE_GRAPH request through it.
 *
 * Usage: analyze_graph_c_client <host> <port>
 * Exit 0 on a valid ACK RESPONSE for the expected request_id, exit 1 otherwise.
 */

#include "eop_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* cmd_type 255 in eop_client.c means "send this payload verbatim" -- needed here since
 * ANALYZE_GRAPH is a raw JSON envelope, not one of the fixed REGISTER/QUERY_NODE/etc. commands
 * the library has a short enum for. */
#define EOP_CMD_RAW       255
#define EOP_CMD_HEARTBEAT 6
/* eop-server dispatches ANALYZE_GRAPH asynchronously (real round trip to hpc-engine) while a
 * HEARTBEAT gets ACKed inline and near-instantly -- confirmed live: a tight retry loop of plain
 * HEARTBEATs races ahead and exhausts a small attempt budget before the slower response is even
 * enqueued. A short sleep per attempt gives the dispatch real wall-clock time to complete. */
#define MAX_FLUSH_ATTEMPTS   50
#define FLUSH_RETRY_DELAY_US 20000

static const char* kRequestId = "c-client-e2e";

/**
 * eop_send_command() does one raw recv() per call with no line-buffering or request/response
 * correlation: whatever bytes happen to be sitting in the kernel receive buffer at that moment
 * come back, even if they're the reply to an earlier, already-sent command (confirmed live:
 * eop_connect() also never reads/discards the server's unsolicited WELCOME banner, so the very
 * first read after connecting is already one behind). On a single connection responses still
 * arrive in FIFO order, but ANALYZE_GRAPH's reply is slower than a HEARTBEAT's (it makes a real
 * round trip to hpc-engine), so a naive send-then-immediately-recv pairing can reliably read
 * back a stale, already-queued response instead of the one that was just requested.
 *
 * The protocol already carries a request_id specifically to correlate responses -- read in a
 * small retry loop and check each response for the expected request_id, rather than assuming a
 * fixed number of reads lines up with the command that was just sent.
 */
static int drainUntilRequestId(eop_client_t* client, const char* requestId, char* outBody, size_t outBodyLen)
{
    for (int attempt = 0; attempt < MAX_FLUSH_ATTEMPTS; ++attempt)
    {
        eop_response_t* resp = eop_send_command(client, EOP_CMD_HEARTBEAT, NULL, 0, 5000);
        if (!resp)
        {
            return 0;
        }
        if (resp->status_code != EOP_OK || !resp->payload)
        {
            eop_response_free(resp);
            continue;
        }

        const char* body = (const char*)resp->payload;
        const int found = strstr(body, requestId) != NULL;
        if (found)
        {
            snprintf(outBody, outBodyLen, "%s", body);
        }
        eop_response_free(resp);

        if (found)
        {
            return 1;
        }

        usleep(FLUSH_RETRY_DELAY_US);
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <host> <port>\n", argv[0]);
        return 2;
    }

    const char* host = argv[1];
    const uint16_t port = (uint16_t)atoi(argv[2]);

    eop_client_t* client = eop_connect(host, port, 5000);
    if (!client)
    {
        fprintf(stderr, "ERROR: eop_connect failed: %s\n", eop_last_error());
        return 1;
    }

    /* Small on purpose: stays well under MESSAGE_BUFFER_SIZE (256 bytes) server-side, so the
     * whole line always lands in a single recv() -- see the framing note above eop_send_command
     * in eop_client.c: it never appends the protocol's '\n' terminator itself, so callers must
     * supply one, and messages that don't fit in one recv() are mishandled server-side
     * regardless (tracked separately). */
    char payload[256];
    snprintf(payload,
             sizeof(payload),
             "{\"type\":\"ANALYZE_GRAPH\",\"request_id\":\"%s\",\"payload\":{\"nodes\":[\"A\","
             "\"B\"],\"edges\":[{\"from\":\"A\",\"to\":\"B\",\"weight\":1}],\"algorithms\":["
             "\"connected_components\"]}}\n",
             kRequestId);

    eop_response_t* sendResp = eop_send_command(client, EOP_CMD_RAW, (const uint8_t*)payload, strlen(payload), 5000);
    if (sendResp)
    {
        eop_response_free(sendResp);
    }

    char body[2048] = {0};
    int exitCode = 1;

    if (!drainUntilRequestId(client, kRequestId, body, sizeof(body)))
    {
        fprintf(
            stderr, "ERROR: never saw a response for request_id=%s within %d reads\n", kRequestId, MAX_FLUSH_ATTEMPTS);
    }
    else
    {
        printf("%s\n", body);

        if (strstr(body, "\"type\":\"RESPONSE\"") && strstr(body, "\"status\":\"ACK\""))
        {
            printf("OK: C client -> eop_server -> hpc_engine ANALYZE_GRAPH flow works\n");
            exitCode = 0;
        }
        else
        {
            fprintf(stderr, "ERROR: unexpected response shape\n");
        }
    }

    eop_disconnect(client);

    return exitCode;
}
