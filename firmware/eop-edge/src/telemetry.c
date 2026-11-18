#include "eop_telemetry.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>

#define MS_PER_SECOND     1000
#define SECONDS_PER_DAY   86400
#define SECONDS_PER_HOUR  3600
#define SECONDS_PER_MIN   60
#define TENTHS_PER_DEGREE 10

static const char *const EVENT_NAMES[] = {
	[EOP_EVENT_BOOT] = "BOOT",
	[EOP_EVENT_RECONNECTED] = "RECONNECTED",
};

static const char *const STATUS_NAMES[] = {
	[EOP_STATUS_OK] = "OK",
	[EOP_STATUS_WARN] = "WARN",
	[EOP_STATUS_ALERT] = "ALERT",
};

const char *eop_status_name(enum eop_status status)
{
	if ((unsigned int)status >= ARRAY_SIZE(STATUS_NAMES)) {
		return STATUS_NAMES[EOP_STATUS_OK];
	}
	return STATUS_NAMES[status];
}

enum eop_status eop_status_for(int16_t temperature_dc, int16_t warn_dc, int16_t alert_dc)
{
	if (temperature_dc >= alert_dc) {
		return EOP_STATUS_ALERT;
	}
	if (temperature_dc >= warn_dc) {
		return EOP_STATUS_WARN;
	}
	return EOP_STATUS_OK;
}

static bool name_valid(const char *name, bool (*allowed)(char))
{
	const char *end = memchr(name, '\0', EOP_NAME_MAX_LEN + 1);

	if (end == NULL || end == name) {
		return false;
	}
	size_t len = (size_t)(end - name);

	if (len > EOP_NAME_MAX_LEN) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (!allowed(name[i])) {
			return false;
		}
	}
	return true;
}

static bool node_id_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

static bool bunker_name_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	       c == '_' || c == '.' || c == '-';
}

bool eop_node_id_valid(const char *node_id)
{
	return name_valid(node_id, node_id_char) &&
	       strncmp(node_id, EOP_RESERVED_NODE_PREFIX, strlen(EOP_RESERVED_NODE_PREFIX)) != 0;
}

bool eop_bunker_name_valid(const char *bunker_name)
{
	return name_valid(bunker_name, bunker_name_char);
}

/* Civil date from days since 1970-01-01 (H. Hinnant's days_from_civil, inverted); exact for
 * every date the device can see, with no dependency on the C library's time functions.
 */
static void civil_from_days(int64_t days, int *year, int *month, int *day)
{
	const int64_t DAYS_PER_ERA = 146097;
	const int64_t EPOCH_SHIFT = 719468; /* 1970-01-01 to 0000-03-01 */
	int64_t z = days + EPOCH_SHIFT;
	int64_t era = (z >= 0 ? z : z - (DAYS_PER_ERA - 1)) / DAYS_PER_ERA;
	int64_t doe = z - era * DAYS_PER_ERA;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	int64_t d = doy - (153 * mp + 2) / 5 + 1;
	int64_t m = mp < 10 ? mp + 3 : mp - 9;

	*year = (int)(yoe + era * 400 + (m <= 2 ? 1 : 0));
	*month = (int)m;
	*day = (int)d;
}

int eop_format_timestamp(int64_t epoch_ms, char *buf, size_t len)
{
	int year, month, day;

	if (epoch_ms < 0) {
		return -EINVAL;
	}
	if (len < EOP_TIMESTAMP_LEN + 1) {
		return -ENOSPC;
	}

	int64_t seconds = epoch_ms / MS_PER_SECOND;
	int64_t in_day = seconds % SECONDS_PER_DAY;

	civil_from_days(seconds / SECONDS_PER_DAY, &year, &month, &day);
	int written = snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", year, month, day,
			       (int)(in_day / SECONDS_PER_HOUR),
			       (int)(in_day % SECONDS_PER_HOUR / SECONDS_PER_MIN),
			       (int)(in_day % SECONDS_PER_MIN), (int)(epoch_ms % MS_PER_SECOND));

	return written == EOP_TIMESTAMP_LEN ? written : -ENOSPC;
}

/* snprintf result to the encoders' return convention. */
static int written_or_nospc(int written, size_t len)
{
	if (written < 0 || (size_t)written >= len) {
		return -ENOSPC;
	}
	return written;
}

int eop_telemetry_encode(const struct eop_telemetry *sample, const struct eop_device_config *config,
			 char *buf, size_t len)
{
	char timestamp[EOP_TIMESTAMP_LEN + 1];
	int ret = eop_format_timestamp(sample->sampled_at_ms, timestamp, sizeof(timestamp));

	if (ret < 0) {
		return ret;
	}

	/* Tenths printed as integer and digit so no floating point printf is needed; the sign is
	 * written separately because -0.5 has an integer part of 0.
	 */
	int abs_dc = sample->temperature_dc < 0 ? -sample->temperature_dc : sample->temperature_dc;
	int written = snprintf(buf, len,
			       "{\"node_id\":\"%s\",\"bunker_name\":\"%s\",\"timestamp\":\"%s\","
			       "\"temperature\":%s%d.%d,\"status\":\"%s\",\"message_id\":%u,"
			       "\"boot_id\":\"%s\"}",
			       config->node_id, config->bunker_name, timestamp,
			       sample->temperature_dc < 0 ? "-" : "", abs_dc / TENTHS_PER_DEGREE,
			       abs_dc % TENTHS_PER_DEGREE, eop_status_name(sample->status),
			       (unsigned int)sample->message_id, config->boot_id);

	return written_or_nospc(written, len);
}

int eop_event_encode(enum eop_event event, uint32_t delivered, int64_t now_ms,
		     const struct eop_device_config *config, char *buf, size_t len)
{
	char timestamp[EOP_TIMESTAMP_LEN + 1];
	int ret = eop_format_timestamp(now_ms, timestamp, sizeof(timestamp));

	if (ret < 0) {
		return ret;
	}
	if ((unsigned int)event >= ARRAY_SIZE(EVENT_NAMES)) {
		return -EINVAL;
	}

	/* Only RECONNECTED carries a count. */
	char tail[sizeof(",\"delivered\":4294967295}")] = "}";

	if (event == EOP_EVENT_RECONNECTED) {
		(void)snprintf(tail, sizeof(tail), ",\"delivered\":%u}", (unsigned int)delivered);
	}
	int written =
		snprintf(buf, len,
			 "{\"node_id\":\"%s\",\"boot_id\":\"%s\",\"timestamp\":\"%s\","
			 "\"event\":\"%s\"%s",
			 config->node_id, config->boot_id, timestamp, EVENT_NAMES[event], tail);

	return written_or_nospc(written, len);
}

int eop_health_encode(const struct eop_health *health, int64_t now_ms,
		      const struct eop_device_config *config, char *buf, size_t len)
{
	char timestamp[EOP_TIMESTAMP_LEN + 1];
	int ret = eop_format_timestamp(now_ms, timestamp, sizeof(timestamp));

	if (ret < 0) {
		return ret;
	}

	int written = snprintf(
		buf, len,
		"{\"node_id\":\"%s\",\"boot_id\":\"%s\",\"timestamp\":\"%s\",\"uptime_s\":%u,"
		"\"restarts\":{\"sensor\":%u,\"mqtt\":%u},\"buffered\":%u,\"dropped\":%u}",
		config->node_id, config->boot_id, timestamp, (unsigned int)health->uptime_s,
		(unsigned int)health->restarts_sensor, (unsigned int)health->restarts_mqtt,
		(unsigned int)health->buffered, (unsigned int)health->dropped);

	return written_or_nospc(written, len);
}
