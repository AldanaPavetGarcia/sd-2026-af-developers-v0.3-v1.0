#ifndef EOP_CLIENT_H
#define EOP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define EOP_OK                    0
#define EOP_ERR_DISCONNECTED      -1
#define EOP_ERR_TIMEOUT           -2
#define EOP_ERR_CONNECTION_FAILED -3
#define EOP_ERR_INVALID_ARGUMENT  -4
#define EOP_ERR_MEMORY            -5
#define EOP_ERR_INTERNAL          -6

    /**
     * @brief Opaque handler for the EOP client.
     */
    typedef struct eop_client eop_client_t;

    /**
     * @brief Response structure for EOP commands.
     */
    typedef struct
    {
        int status_code;
        uint8_t* payload;
        size_t payload_len;
    } eop_response_t;

    /**
     * @brief Connects to an EOP server.
     *
     * @param host The hostname or IP address to connect to.
     * @param port The port number.
     * @param timeout_ms Connection timeout in milliseconds.
     * @return eop_client_t* Opaque handler on success, NULL on failure.
     */
    eop_client_t* eop_connect(const char* host, uint16_t port, uint32_t timeout_ms);

    /**
     * @brief Disconnects from the EOP server and frees the client resources.
     *
     * This function releases all resources owned by the opaque client handle.
     * Passing NULL is a safe no-op.
     *
     * After this function returns, the client handle is no longer valid. Using the
     * same handle again, including passing it to eop_send_command() or calling
     * eop_disconnect() twice with the same pointer, is undefined behavior.
     *
     * @param client The client handler. Safe to call with NULL.
     */
    void eop_disconnect(eop_client_t* client);

    /**
     * @brief Retrieves the last error message for the calling thread.
     *
     * @return const char* Human-readable error description.
     */
    const char* eop_last_error(void);

    /**
     * @brief Sends a command to the server and waits for a response.
     *
     * @param client The client handler.
     * @param cmd_type The command type.
     * @param payload The payload to send.
     * @param len The length of the payload.
     * @param timeout_ms Timeout for the command in milliseconds.
     * @return eop_response_t* Response object. The caller must free it using eop_response_free().
     *         Returns an object with status_code EOP_ERR_DISCONNECTED or EOP_ERR_TIMEOUT if failed.
     */
    eop_response_t*
    eop_send_command(eop_client_t* client, uint8_t cmd_type, const uint8_t* payload, size_t len, uint32_t timeout_ms);

    /**
     * @brief Frees an eop_response_t object.
     *
     * @param response The response to free. Safe to call with NULL.
     */
    void eop_response_free(eop_response_t* response);

#ifdef __cplusplus
}
#endif

#endif // EOP_CLIENT_H
