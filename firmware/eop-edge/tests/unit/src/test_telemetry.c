#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "eop_telemetry.h"

/* 2026-11-02T14:03:07.120Z, the example of docs/edge-telemetry-protocol-v1.0.md §2.1. */
#define EXAMPLE_MS 1793628187120LL

static const struct eop_device_config CONFIG = {
	.node_id = "vault-101",
	.bunker_name = "Vault-101",
	.boot_id = "9f3c27a1",
	.broker_host = "mosquitto",
	.broker_port = 1883,
};

ZTEST_SUITE(telemetry, NULL, NULL, NULL, NULL, NULL);

ZTEST(telemetry, test_encodes_the_protocol_example)
{
	struct eop_telemetry sample = {
		.message_id = 4182,
		.sampled_at_ms = EXAMPLE_MS,
		.temperature_dc = 214,
		.status = EOP_STATUS_OK,
	};
	char buf[256];
	const char *expected =
		"{\"node_id\":\"vault-101\",\"bunker_name\":\"Vault-101\","
		"\"timestamp\":\"2026-11-02T14:03:07.120Z\",\"temperature\":21.4,\"status\":\"OK\","
		"\"message_id\":4182,\"boot_id\":\"9f3c27a1\"}";

	int len = eop_telemetry_encode(&sample, &CONFIG, buf, sizeof(buf));

	zassert_equal(len, (int)strlen(expected));
	zassert_str_equal(buf, expected);
}

ZTEST(telemetry, test_temperatures_keep_sign_and_tenth)
{
	struct eop_telemetry sample = {.message_id = 1, .sampled_at_ms = EXAMPLE_MS};
	char buf[256];
	const struct {
		int16_t dc;
		const char *text;
	} cases[] = {
		{0, "\"temperature\":0.0,"},      {5, "\"temperature\":0.5,"},
		{-5, "\"temperature\":-0.5,"},    {-214, "\"temperature\":-21.4,"},
		{1500, "\"temperature\":150.0,"}, {-500, "\"temperature\":-50.0,"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		sample.temperature_dc = cases[i].dc;
		zassert_true(eop_telemetry_encode(&sample, &CONFIG, buf, sizeof(buf)) > 0);
		zassert_not_null(strstr(buf, cases[i].text), "%d -> %s", cases[i].dc, buf);
	}
}

ZTEST(telemetry, test_short_buffer_is_rejected_not_truncated)
{
	struct eop_telemetry sample = {.message_id = 1, .sampled_at_ms = EXAMPLE_MS};
	char buf[64];

	zassert_equal(eop_telemetry_encode(&sample, &CONFIG, buf, sizeof(buf)), -ENOSPC);
}

ZTEST(telemetry, test_sample_before_the_epoch_is_rejected)
{
	/* A device whose clock was never set must not publish 1970 timestamps as valid. */
	struct eop_telemetry sample = {.message_id = 1, .sampled_at_ms = -1};
	char buf[256];

	zassert_equal(eop_telemetry_encode(&sample, &CONFIG, buf, sizeof(buf)), -EINVAL);
}

ZTEST(telemetry, test_timestamps_across_calendar_edges)
{
	char buf[EOP_TIMESTAMP_LEN + 1];
	const struct {
		int64_t ms;
		const char *text;
	} cases[] = {
		{0, "1970-01-01T00:00:00.000Z"},
		{951782400000LL, "2000-02-29T00:00:00.000Z"},  /* leap day of a /400 year */
		{1709251199999LL, "2024-02-29T23:59:59.999Z"}, /* last ms of a leap day */
		{1798761599999LL, "2026-12-31T23:59:59.999Z"},
		{4102444800000LL, "2100-01-01T00:00:00.000Z"}, /* 2100 is not a leap year */
	};

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		zassert_equal(eop_format_timestamp(cases[i].ms, buf, sizeof(buf)),
			      EOP_TIMESTAMP_LEN);
		zassert_str_equal(buf, cases[i].text);
	}
	zassert_equal(eop_format_timestamp(0, buf, EOP_TIMESTAMP_LEN), -ENOSPC);
}

ZTEST(telemetry, test_status_thresholds)
{
	zassert_equal(eop_status_for(299, 300, 350), EOP_STATUS_OK);
	zassert_equal(eop_status_for(300, 300, 350), EOP_STATUS_WARN);
	zassert_equal(eop_status_for(349, 300, 350), EOP_STATUS_WARN);
	zassert_equal(eop_status_for(350, 300, 350), EOP_STATUS_ALERT);
	zassert_str_equal(eop_status_name(EOP_STATUS_ALERT), "ALERT");
	zassert_str_equal(eop_status_name((enum eop_status)42), "OK");
}

ZTEST(telemetry, test_node_id_rules)
{
	char longest[EOP_NAME_MAX_LEN + 2];

	zassert_true(eop_node_id_valid("vault-101"));
	zassert_true(eop_node_id_valid("a"));
	zassert_false(eop_node_id_valid(""));
	zassert_false(eop_node_id_valid("Vault-101"), "upper case");
	zassert_false(eop_node_id_valid("vault 101"), "space");
	zassert_false(eop_node_id_valid("vault/101"), "would add a topic level");
	zassert_false(eop_node_id_valid("hpc-engine-1"), "reserved prefix (ADR-022)");
	zassert_true(eop_node_id_valid("my-hpc-engine"), "only the prefix is reserved");

	memset(longest, 'a', EOP_NAME_MAX_LEN);
	longest[EOP_NAME_MAX_LEN] = '\0';
	zassert_true(eop_node_id_valid(longest));
	longest[EOP_NAME_MAX_LEN] = 'a';
	longest[EOP_NAME_MAX_LEN + 1] = '\0';
	zassert_false(eop_node_id_valid(longest), "64 characters");
}

ZTEST(telemetry, test_bunker_name_rules)
{
	zassert_true(eop_bunker_name_valid("Vault-101"));
	zassert_true(eop_bunker_name_valid("Vault_101.B"));
	zassert_false(eop_bunker_name_valid(""));
	zassert_false(eop_bunker_name_valid("Vault 101"), "space breaks key=value lines");
	zassert_false(eop_bunker_name_valid("Vault=101"));
	zassert_false(eop_bunker_name_valid("Vault\"101"), "would break the JSON payload");
}

/* Protocol §2.2. */
ZTEST(telemetry, test_encodes_events)
{
	char buf[256];
	const char *boot = "{\"node_id\":\"vault-101\",\"boot_id\":\"9f3c27a1\","
			   "\"timestamp\":\"2026-11-02T14:03:07.120Z\",\"event\":\"BOOT\"}";
	const char *reconnected = "{\"node_id\":\"vault-101\",\"boot_id\":\"9f3c27a1\","
				  "\"timestamp\":\"2026-11-02T14:03:07.120Z\",\"event\":"
				  "\"RECONNECTED\",\"delivered\":37}";

	zassert_equal(eop_event_encode(EOP_EVENT_BOOT, 37, EXAMPLE_MS, &CONFIG, buf, sizeof(buf)),
		      (int)strlen(boot));
	zassert_str_equal(buf, boot, "BOOT carries no count");
	zassert_equal(
		eop_event_encode(EOP_EVENT_RECONNECTED, 37, EXAMPLE_MS, &CONFIG, buf, sizeof(buf)),
		(int)strlen(reconnected));
	zassert_str_equal(buf, reconnected);
}

ZTEST(telemetry, test_event_errors)
{
	char buf[256];
	char small[40];

	zassert_equal(eop_event_encode((enum eop_event)7, 0, EXAMPLE_MS, &CONFIG, buf, sizeof(buf)),
		      -EINVAL);
	zassert_equal(eop_event_encode(EOP_EVENT_BOOT, 0, -1, &CONFIG, buf, sizeof(buf)), -EINVAL);
	zassert_equal(eop_event_encode(EOP_EVENT_RECONNECTED, UINT32_MAX, EXAMPLE_MS, &CONFIG,
				       small, sizeof(small)),
		      -ENOSPC);
}

/* Protocol §2.3: the dropped count is the overflow metric of US-402 AC8. */
ZTEST(telemetry, test_encodes_health)
{
	char buf[256];
	struct eop_health health = {
		.uptime_s = 3600,
		.restarts_sensor = 0,
		.restarts_mqtt = 1,
		.buffered = 12,
		.dropped = 5,
	};
	const char *expected =
		"{\"node_id\":\"vault-101\",\"boot_id\":\"9f3c27a1\","
		"\"timestamp\":\"2026-11-02T14:03:07.120Z\",\"uptime_s\":3600,"
		"\"restarts\":{\"sensor\":0,\"mqtt\":1},\"buffered\":12,\"dropped\":5}";

	zassert_equal(eop_health_encode(&health, EXAMPLE_MS, &CONFIG, buf, sizeof(buf)),
		      (int)strlen(expected));
	zassert_str_equal(buf, expected);
	zassert_equal(eop_health_encode(&health, EXAMPLE_MS, &CONFIG, buf, strlen(expected)),
		      -ENOSPC, "no room for the terminator");
	zassert_equal(eop_health_encode(&health, -1, &CONFIG, buf, sizeof(buf)), -EINVAL);
}

/* Default of CONFIG_EOP_PAYLOAD_MAX_LEN (the unit tests do not load the firmware's Kconfig). */
#define PAYLOAD_MAX_LEN 320

/* The longest identity still fits the payload buffer. */
ZTEST(telemetry, test_longest_payloads_fit)
{
	struct eop_device_config wide = CONFIG;
	struct eop_health health = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
	struct eop_telemetry sample = {UINT32_MAX, EXAMPLE_MS, -500, EOP_STATUS_ALERT};
	char buf[PAYLOAD_MAX_LEN];

	memset(wide.node_id, 'a', EOP_NAME_MAX_LEN);
	wide.node_id[EOP_NAME_MAX_LEN] = '\0';
	memset(wide.bunker_name, 'B', EOP_NAME_MAX_LEN);
	wide.bunker_name[EOP_NAME_MAX_LEN] = '\0';

	zassert_true(eop_telemetry_encode(&sample, &wide, buf, sizeof(buf)) > 0);
	zassert_true(eop_health_encode(&health, EXAMPLE_MS, &wide, buf, sizeof(buf)) > 0);
	zassert_true(eop_event_encode(EOP_EVENT_RECONNECTED, UINT32_MAX, EXAMPLE_MS, &wide, buf,
				      sizeof(buf)) > 0);
}
