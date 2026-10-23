/**
 * @file eop_status.h
 * @brief Device status shared by the threads and shown by the shell (US-401 AC8).
 * @details The MQTT thread writes the connection state and publication counters; the shell
 *          command `eop status` reads them together with the queue counters.
 */

#ifndef EOP_STATUS_H
#define EOP_STATUS_H

#include <stdint.h>

#include <zephyr/kernel.h>

#include "eop_queue.h"

/** Broker connection state. */
enum eop_conn_state {
	EOP_CONN_DISCONNECTED, /**< No connection; waiting for the next attempt. */
	EOP_CONN_CONNECTING,   /**< TCP connect or CONNACK pending. */
	EOP_CONN_CONNECTED,    /**< CONNACK accepted. */
};

/** Status of the device since boot. */
struct eop_status_board {
	struct k_mutex lock;            /**< Guards every field below. */
	enum eop_conn_state conn_state; /**< Current broker connection state. */
	int64_t last_published_ms; /**< Wall clock of the last acknowledged sample, 0 if none. */
	uint32_t messages_sent;    /**< Samples acknowledged by the broker since boot. */
	uint32_t connect_failures; /**< Failed connection attempts since boot. */
	int64_t first_puback_uptime_ms; /**< Uptime of the first PUBACK; -1 before (AC6). */
	struct eop_queue *queue;        /**< Queue whose counters the status shows. */
};

/**
 * @brief Prepares the status board.
 * @param board Board to initialise.
 * @param queue Telemetry queue, for its buffered and dropped counters.
 */
void eop_status_init(struct eop_status_board *board, struct eop_queue *queue);

/**
 * @brief Records a change of connection state.
 * @param board Status board.
 * @param state New state.
 */
void eop_status_set_conn(struct eop_status_board *board, enum eop_conn_state state);

/**
 * @brief Records a failed connection attempt.
 * @param board Status board.
 */
void eop_status_connect_failed(struct eop_status_board *board);

/**
 * @brief Records a sample acknowledged by the broker.
 * @param board Status board.
 * @param sampled_at_ms Wall clock of the sample.
 * @return true if it was the first acknowledgement since boot.
 */
bool eop_status_published(struct eop_status_board *board, int64_t sampled_at_ms);

/** Consistent copy of the status board, for printing. */
struct eop_status_snapshot {
	enum eop_conn_state conn_state;
	int64_t last_published_ms;
	uint32_t messages_sent;
	uint32_t connect_failures;
	uint32_t buffered;
	uint32_t dropped;
};

/**
 * @brief Takes a consistent copy of the status.
 * @param board Status board.
 * @param out Where the copy is written.
 */
void eop_status_snapshot(struct eop_status_board *board, struct eop_status_snapshot *out);

/**
 * @brief Name of a connection state, as the shell prints it.
 * @param state State.
 * @return "disconnected", "connecting" or "connected".
 */
const char *eop_conn_state_name(enum eop_conn_state state);

/**
 * @brief Makes @p board the one the `eop status` shell command prints.
 * @param board Status board; must outlive the shell.
 */
void eop_shell_attach(struct eop_status_board *board);

#endif /* EOP_STATUS_H */
