/*
 * MQTT thread (ADR-014): owns the broker connection, takes samples from the queue in FIFO order
 * and publishes each with QoS 1, waiting for its PUBACK before the next one. A sample whose
 * PUBACK did not arrive stays pending and is sent first after reconnecting, so nothing taken out
 * of the queue is lost. Reconnection uses exponential backoff (US-401 AC5, US-402 AC5).
 *
 * While offline the queue is the buffer (US-402 AC1). After a reconnection the buffered samples go
 * out first, oldest first, because the queue is FIFO (AC2); once the newest of them is
 * acknowledged, RECONNECTED is published to …/events with how many were delivered (AC6). The
 * first connection since boot publishes BOOT instead. While connected, a health report goes to
 * …/health every CONFIG_EOP_HEALTH_PERIOD_MS, with the dropped count (AC8).
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>

#include "eop_backoff.h"
#include "eop_flush.h"
#include "eop_platform.h"
#include "eop_threads.h"

LOG_MODULE_REGISTER(eop_mqtt, LOG_LEVEL_INF);

#define TOPIC_SUFFIX_TELEMETRY "telemetry"
#define TOPIC_SUFFIX_EVENTS    "events"
#define TOPIC_SUFFIX_HEALTH    "health"
/* The telemetry suffix is the longest, so it sizes every topic buffer. */
#define TOPIC_MAX_LEN                                                                              \
	(sizeof(CONFIG_EOP_MQTT_TOPIC_PREFIX) + EOP_NAME_MAX_LEN +                                 \
	 sizeof(TOPIC_SUFFIX_TELEMETRY) + 1)
#define PORT_STR_LEN    6
#define FIRST_PACKET_ID 1U
#define MS_PER_SECOND   1000

struct mqtt_session {
	struct mqtt_client client;
	struct sockaddr_storage broker;
	uint8_t rx_buf[CONFIG_EOP_MQTT_BUFFER_SIZE];
	uint8_t tx_buf[CONFIG_EOP_MQTT_BUFFER_SIZE];
	char payload[CONFIG_EOP_PAYLOAD_MAX_LEN];
	char topic_telemetry[TOPIC_MAX_LEN];
	char topic_events[TOPIC_MAX_LEN];
	char topic_health[TOPIC_MAX_LEN];
	struct eop_context *ctx;
	bool connected;
	bool connack_seen;
	int connack_result;
	uint16_t packet_id;
	uint16_t awaiting_id;
	bool puback_seen;
};

static void evt_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	struct mqtt_session *s = client->user_data;

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		s->connack_seen = true;
		s->connack_result = evt->result;
		s->connected = evt->result == 0;
		break;
	case MQTT_EVT_DISCONNECT:
		s->connected = false;
		break;
	case MQTT_EVT_PUBACK:
		if (evt->result == 0 && evt->param.puback.message_id == s->awaiting_id) {
			s->puback_seen = true;
		}
		break;
	default:
		break;
	}
}

static int resolve_broker(struct mqtt_session *s)
{
	const struct eop_device_config *config = s->ctx->config;
	struct zsock_addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
	struct zsock_addrinfo *result;
	char port[PORT_STR_LEN];

	(void)snprintf(port, sizeof(port), "%u", config->broker_port);
	int ret = zsock_getaddrinfo(config->broker_host, port, &hints, &result);

	if (ret != 0) {
		return -EHOSTUNREACH;
	}
	memcpy(&s->broker, result->ai_addr, MIN(result->ai_addrlen, sizeof(s->broker)));
	zsock_freeaddrinfo(result);
	return 0;
}

/* Feeds broker input to the MQTT library until *flag is set, the connection drops or the time
 * runs out. Returns 0 when the flag was set.
 */
static int wait_for(struct mqtt_session *s, const bool *flag, int32_t timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (!*flag) {
		int64_t left = deadline - k_uptime_get();

		if (left <= 0) {
			return -ETIMEDOUT;
		}
		struct zsock_pollfd fds = {.fd = s->client.transport.tcp.sock,
					   .events = ZSOCK_POLLIN};
		int ready = zsock_poll(&fds, 1, (int)MIN(left, CONFIG_EOP_MQTT_POLL_MS));

		if (ready < 0) {
			return -errno;
		}
		if (ready > 0) {
			if ((fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLNVAL)) != 0) {
				return -ECONNRESET;
			}
			int ret = mqtt_input(&s->client);

			if (ret < 0) {
				return ret;
			}
		}
	}
	return 0;
}

static void drop_connection(struct mqtt_session *s)
{
	(void)mqtt_abort(&s->client);
	s->connected = false;
	eop_status_set_conn(s->ctx->status, EOP_CONN_DISCONNECTED);
}

static int broker_connect(struct mqtt_session *s)
{
	const struct eop_device_config *config = s->ctx->config;
	struct mqtt_client *client = &s->client;
	int ret;

	eop_status_set_conn(s->ctx->status, EOP_CONN_CONNECTING);
	ret = resolve_broker(s);
	if (ret < 0) {
		return ret;
	}

	mqtt_client_init(client);
	client->broker = &s->broker;
	client->evt_cb = evt_handler;
	client->user_data = s;
	client->client_id.utf8 = (const uint8_t *)config->node_id;
	client->client_id.size = strlen(config->node_id);
	client->protocol_version = MQTT_VERSION_3_1_1;
	/* A persistent session keeps QoS 1 state across reconnections (ADR-013). */
	client->clean_session = 0;
	client->keepalive = CONFIG_EOP_MQTT_KEEPALIVE_S;
	client->rx_buf = s->rx_buf;
	client->rx_buf_size = sizeof(s->rx_buf);
	client->tx_buf = s->tx_buf;
	client->tx_buf_size = sizeof(s->tx_buf);
	client->transport.type = MQTT_TRANSPORT_NON_SECURE;

	s->connack_seen = false;
	ret = mqtt_connect(client);
	if (ret < 0) {
		return ret;
	}
	ret = wait_for(s, &s->connack_seen, CONFIG_EOP_MQTT_CONNECT_TIMEOUT_MS);
	if (ret < 0 || s->connack_result != 0) {
		(void)mqtt_abort(client);
		return ret < 0 ? ret : -ECONNREFUSED;
	}
	eop_status_set_conn(s->ctx->status, EOP_CONN_CONNECTED);
	return 0;
}

static uint16_t next_packet_id(struct mqtt_session *s)
{
	/* MQTT packet ids are 1..65535; 0 is not allowed. */
	s->packet_id = s->packet_id == UINT16_MAX ? FIRST_PACKET_ID : s->packet_id + 1U;
	return s->packet_id;
}

/* Publishes s->payload[0..len) with QoS 1 and waits for its PUBACK. */
static int publish(struct mqtt_session *s, const char *topic, int len)
{
	if (len < 0) {
		return len;
	}

	struct mqtt_publish_param param = {
		.message =
			{
				.topic =
					{
						.topic = {.utf8 = (const uint8_t *)topic,
							  .size = strlen(topic)},
						.qos = MQTT_QOS_1_AT_LEAST_ONCE,
					},
				.payload = {.data = (uint8_t *)s->payload, .len = (uint32_t)len},
			},
		.message_id = next_packet_id(s),
	};

	s->awaiting_id = param.message_id;
	s->puback_seen = false;
	int ret = mqtt_publish(&s->client, &param);

	if (ret < 0) {
		return ret;
	}
	return wait_for(s, &s->puback_seen, CONFIG_EOP_MQTT_ACK_TIMEOUT_MS);
}

static int publish_sample(struct mqtt_session *s, const struct eop_telemetry *sample)
{
	return publish(
		s, s->topic_telemetry,
		eop_telemetry_encode(sample, s->ctx->config, s->payload, sizeof(s->payload)));
}

static int publish_event(struct mqtt_session *s, enum eop_event event, uint32_t delivered)
{
	return publish(s, s->topic_events,
		       eop_event_encode(event, delivered, eop_platform_now_ms(), s->ctx->config,
					s->payload, sizeof(s->payload)));
}

static int publish_health(struct mqtt_session *s)
{
	struct eop_health health = {
		.uptime_s = (uint32_t)(k_uptime_get() / MS_PER_SECOND),
		/* Thread restarts come with the watchdog (US-403). */
		.restarts_sensor = 0,
		.restarts_mqtt = 0,
		.buffered = eop_queue_count(s->ctx->queue),
		.dropped = eop_queue_dropped(s->ctx->queue),
	};

	return publish(s, s->topic_health,
		       eop_health_encode(&health, eop_platform_now_ms(), s->ctx->config, s->payload,
					 sizeof(s->payload)));
}

/* Reads pending input (PINGRESP) and sends a PINGREQ when the keep-alive requires it. */
static int keep_alive(struct mqtt_session *s)
{
	struct zsock_pollfd fds = {.fd = s->client.transport.tcp.sock, .events = ZSOCK_POLLIN};
	int ready = zsock_poll(&fds, 1, 0);

	if (ready < 0) {
		return -errno;
	}
	if (ready > 0) {
		if ((fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLNVAL)) != 0) {
			return -ECONNRESET;
		}
		int ret = mqtt_input(&s->client);

		if (ret < 0) {
			return ret;
		}
	}
	int ret = mqtt_live(&s->client);

	return ret == -EAGAIN ? 0 : ret;
}

static int build_topic(struct mqtt_session *s, char *topic, const char *suffix)
{
	int len = snprintf(topic, TOPIC_MAX_LEN, "%s/%s/%s", CONFIG_EOP_MQTT_TOPIC_PREFIX,
			   s->ctx->config->node_id, suffix);

	return len < 0 || (size_t)len >= TOPIC_MAX_LEN ? -ENOSPC : 0;
}

/* After a connection: BOOT the first time, otherwise start counting the flush; RECONNECTED goes
 * out at once if nothing was buffered.
 */
static int announce_connection(struct mqtt_session *s, struct eop_flush *flush, bool *booted,
			       bool have_pending)
{
	if (!*booted) {
		int ret = publish_event(s, EOP_EVENT_BOOT, 0);

		*booted = ret == 0;
		return ret;
	}
	bool backlog = have_pending || eop_queue_count(s->ctx->queue) > 0;

	if (!eop_flush_start(flush, backlog, eop_queue_newest_id(s->ctx->queue))) {
		return 0;
	}
	eop_status_reconnected(s->ctx->status, 0);
	return publish_event(s, EOP_EVENT_RECONNECTED, 0);
}

void eop_mqtt_thread(void *ctx_ptr, void *unused2, void *unused3)
{
	static struct mqtt_session session;
	struct mqtt_session *s = &session;
	struct eop_backoff backoff;
	struct eop_flush flush = {0};
	struct eop_telemetry pending;
	bool have_pending = false;
	bool booted = false;
	int64_t next_health_ms = 0;

	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);

	memset(s, 0, sizeof(*s));
	s->ctx = ctx_ptr;
	if (build_topic(s, s->topic_telemetry, TOPIC_SUFFIX_TELEMETRY) < 0 ||
	    build_topic(s, s->topic_events, TOPIC_SUFFIX_EVENTS) < 0 ||
	    build_topic(s, s->topic_health, TOPIC_SUFFIX_HEALTH) < 0) {
		LOG_ERR("topics do not fit in %zu bytes", (size_t)TOPIC_MAX_LEN);
		return;
	}
	eop_backoff_init(&backoff, CONFIG_EOP_BACKOFF_INITIAL_S, CONFIG_EOP_BACKOFF_MAX_S);

	for (;;) {
		if (!s->connected) {
			bool link_up = eop_link_is_up(s->ctx->link);
			int ret = link_up ? broker_connect(s) : -ENETDOWN;

			if (ret < 0) {
				uint32_t delay_s = eop_backoff_next(&backoff);

				eop_status_set_conn(s->ctx->status, EOP_CONN_DISCONNECTED);
				eop_status_connect_failed(s->ctx->status);
				if (link_up) {
					LOG_ERR("broker %s:%u unreachable (%d), next attempt in %u "
						"s",
						s->ctx->config->broker_host,
						s->ctx->config->broker_port, ret, delay_s);
				} else {
					LOG_WRN("link down, %u samples buffered, next attempt in "
						"%u s",
						eop_queue_count(s->ctx->queue), delay_s);
				}
				/* A link that comes back cuts the wait short. */
				if (eop_link_wait_up(s->ctx->link, K_SECONDS(delay_s))) {
					LOG_INF("link up: reconnecting now");
				}
				continue;
			}
			eop_backoff_reset(&backoff);
			eop_link_clear(s->ctx->link);
			LOG_INF("connected to %s:%u as %s, publishing to %s",
				s->ctx->config->broker_host, s->ctx->config->broker_port,
				s->ctx->config->node_id, s->topic_telemetry);
			if (announce_connection(s, &flush, &booted, have_pending) < 0) {
				LOG_WRN("connection event not acknowledged; reconnecting");
				drop_connection(s);
				continue;
			}
			next_health_ms = k_uptime_get();
		}

		if (!eop_link_is_up(s->ctx->link)) {
			LOG_WRN("link down: closing the broker connection, buffering samples");
			drop_connection(s);
			continue;
		}

		if (!have_pending) {
			have_pending = eop_queue_get(s->ctx->queue, &pending,
						     K_MSEC(CONFIG_EOP_MQTT_POLL_MS)) == 0;
		}
		if (have_pending) {
			int ret = publish_sample(s, &pending);

			if (ret < 0) {
				LOG_WRN("message %u not acknowledged (%d); reconnecting",
					pending.message_id, ret);
				drop_connection(s);
				continue;
			}
			have_pending = false;
			if (eop_status_published(s->ctx->status, pending.sampled_at_ms)) {
				LOG_INF("first sample acknowledged %lld ms after boot",
					k_uptime_get());
			}
			if (eop_flush_delivered(&flush, pending.message_id)) {
				LOG_INF("reconnected: %u buffered samples delivered",
					flush.delivered);
				eop_status_reconnected(s->ctx->status, flush.delivered);
				if (publish_event(s, EOP_EVENT_RECONNECTED, flush.delivered) < 0) {
					LOG_WRN("RECONNECTED not acknowledged; reconnecting");
					drop_connection(s);
					continue;
				}
			}
		}

		if (k_uptime_get() >= next_health_ms) {
			if (publish_health(s) < 0) {
				LOG_WRN("health report not acknowledged; reconnecting");
				drop_connection(s);
				continue;
			}
			next_health_ms += CONFIG_EOP_HEALTH_PERIOD_MS;
		}

		if (keep_alive(s) < 0) {
			LOG_WRN("broker connection lost; reconnecting");
			drop_connection(s);
		}
	}
}
