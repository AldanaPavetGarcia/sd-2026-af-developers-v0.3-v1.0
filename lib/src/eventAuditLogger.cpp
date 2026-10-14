#include "../include/eventAuditLogger.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <sys/stat.h>

#include <opentelemetry/nostd/span.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>

static std::string getIsoTimestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
       << ms.count() << 'Z';
    return ss.str();
}

static std::string escapeJson(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '"')
            out += "\\\"";
        else if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else if (c >= 0 && c < 32)
        {
            // ignore other control characters
        }
        else
            out += c;
    }
    return out;
}

// ADR-019 log schema: "service" names the emitter so Loki can filter by it (US-306 AC1). Same
// variable the OpenTelemetry SDKs read; read once, the environment does not change at runtime.
static const std::string& serviceName()
{
    static const std::string name = []() -> std::string
    {
        const char* env = std::getenv("OTEL_SERVICE_NAME");
        return (env != nullptr && *env != '\0') ? std::string(env) : std::string(DEFAULT_SERVICE_NAME);
    }();
    return name;
}

EventAuditLogger::EventAuditLogger()
{
    // Ensure logs directory exists
    mkdir(LOGS_DIR, DEFAULT_DIR_PERMISSIONS);
    std::snprintf(logFilePath, sizeof(logFilePath), "%s", SYSTEM_LOG);
}

int EventAuditLogger::writeLog(const char* logMessage, LogType type)
{
    std::lock_guard<std::mutex> lock(loggerMutex);

    if (!ENABLE_LOGGING)
    {
        return 1;
    }

    switch (type)
    {
        case INFO_LOG: std::snprintf(currentEntry.tag, TAG_BUFFER_SIZE, "%s", INFO_TAG); break;
        case CONTROL_LOG: std::snprintf(currentEntry.tag, TAG_BUFFER_SIZE, "%s", CONTROL_TAG); break;
        case ERROR_LOG: std::snprintf(currentEntry.tag, TAG_BUFFER_SIZE, "%s", ERROR_TAG); break;
        case WARNING_LOG: std::snprintf(currentEntry.tag, TAG_BUFFER_SIZE, "%s", WARNING_TAG); break;
        case CRITICAL_LOG: std::snprintf(currentEntry.tag, TAG_BUFFER_SIZE, "%s", CRITICAL_TAG); break;
        case EVENT_LOG: std::snprintf(currentEntry.tag, TAG_BUFFER_SIZE, "%s", EVENT_TAG); break;
        case DEBUG_LOG: std::snprintf(currentEntry.tag, TAG_BUFFER_SIZE, "%s", DEBUG_TAG); break;
        case CONNECTION_LOG: std::snprintf(currentEntry.tag, TAG_BUFFER_SIZE, "%s", REGISTER_TAG); break;
        default: std::snprintf(currentEntry.tag, TAG_BUFFER_SIZE, "%s", DEFAULT_TAG); break;
    }

    std::string levelStr;
    switch (type)
    {
        case INFO_LOG: levelStr = "INFO"; break;
        case CONTROL_LOG: levelStr = "CONTROL"; break;
        case ERROR_LOG: levelStr = "ERROR"; break;
        case WARNING_LOG: levelStr = "WARNING"; break;
        case CRITICAL_LOG: levelStr = "CRITICAL"; break;
        case EVENT_LOG: levelStr = "EVENT"; break;
        case DEBUG_LOG: levelStr = "DEBUG"; break;
        case CONNECTION_LOG: levelStr = "CONNECTION"; break;
        default: levelStr = "LOG"; break;
    }

    std::string traceIdStr;
    std::string spanIdStr;
    auto activeSpan = opentelemetry::trace::Tracer::GetCurrentSpan();
    if (activeSpan)
    {
        auto ctx = activeSpan->GetContext();
        if (ctx.IsValid())
        {
            char traceIdHex[33] = {0};
            ctx.trace_id().ToLowerBase16(opentelemetry::nostd::span<char, 32>(traceIdHex, 32));
            char spanIdHex[17] = {0};
            ctx.span_id().ToLowerBase16(opentelemetry::nostd::span<char, 16>(spanIdHex, 16));
            traceIdStr = std::string(traceIdHex, 32);
            spanIdStr = std::string(spanIdHex, 16);
        }
    }

    std::string nodeIdStr = g_logContext.nodeId;
    std::string eventStr = g_logContext.event;
    if (eventStr.empty())
    {
        eventStr = levelStr + "_EVENT";
    }

    std::cout << "{\"timestamp\":\"" << getIsoTimestamp() << "\",\"level\":\"" << levelStr << "\",\"service\":\""
              << escapeJson(serviceName()) << "\",\"trace_id\":\"" << traceIdStr << "\",\"span_id\":\"" << spanIdStr
              << "\",\"node_id\":\"" << nodeIdStr << "\",\"event\":\"" << eventStr << "\",\"message\":\""
              << escapeJson(logMessage) << "\"}" << std::endl;

    Date dateObj = {};
    Time timeObj = {};
    dateObj.captureDateToString(std::span(currentEntry.entryDate), true);
    currentEntry.separator1 = ' ';
    timeObj.captureTimeToString(std::span(currentEntry.entryTime), true);
    currentEntry.separator2 = ' ';
    std::memset(currentEntry.message, ' ', sizeof(currentEntry.message));
    currentEntry.separator3 = ' ';

    char tempMessage[MESSAGE_BUFFER_SIZE];
    std::strncpy(tempMessage, logMessage, sizeof(tempMessage) - 2);
    tempMessage[sizeof(tempMessage) - 2] = '\0';

    // NOLINTBEGIN(*)
    std::strcpy(currentEntry.message, tempMessage);
    std::strcat(currentEntry.message, "\n");
    // NOLINTEND(*)

    // 1. Write to SYSTEM_LOG (global log)
    int bytesWritten = writeToSpecificLog(logFilePath);

    // 2. If it is a connection log, ALSO write to specific connection logs
    if (type == CONNECTION_LOG)
    {
        writeToSpecificLog(CONNECTIONS_LOG);
        writeToJSON(logMessage);
    }

    return bytesWritten;
}

int EventAuditLogger::writeToSpecificLog(const char* filePath)
{
    int fileHandle = open(filePath, O_RDWR | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fileHandle != -1)
    {
        int remainingBufferOffset = static_cast<int>(MESSAGE_BUFFER_SIZE - std::strlen(currentEntry.message));
        ssize_t written = write(fileHandle,
                                static_cast<const void*>(&currentEntry),
                                sizeof(currentEntry) - static_cast<size_t>(remainingBufferOffset));
        close(fileHandle);
        return static_cast<int>(written);
    }
    return 0;
}

int EventAuditLogger::writeToJSON(const char* logMessage)
{
    char jsonBuffer[512];
    int jsonFileHandle =
        open(REGISTER_CONNECTIONS_LOG, O_RDWR | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    // LCOV_EXCL_START
    if (jsonFileHandle == -1)
    {
        return -1;
    }
    // LCOV_EXCL_STOP

    // JSONL format: one object per line
    int formattedLength = std::snprintf(jsonBuffer,
                                        sizeof(jsonBuffer),
                                        "{\"date\": \"%s\", \"time\": \"%s\", \"tag\": \"%s\", \"message\": \"%s\"}\n",
                                        currentEntry.entryDate,
                                        currentEntry.entryTime,
                                        currentEntry.tag,
                                        logMessage);

    ssize_t bytesWritten = write(jsonFileHandle, jsonBuffer, static_cast<size_t>(formattedLength));
    close(jsonFileHandle);
    return static_cast<int>(bytesWritten);
}

int audit_log_write(const char* logMessage, LogType type)
{
    return auditLogger.writeLog(logMessage, type);
}

int audit_log_write_fmt(LogType type, const char* format, ...)
{
    char buffer[MESSAGE_BUFFER_SIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return auditLogger.writeLog(buffer, type);
}
