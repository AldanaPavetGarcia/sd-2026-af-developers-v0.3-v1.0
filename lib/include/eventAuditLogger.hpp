#ifndef EVENT_AUDIT_LOGGER_HPP
#define EVENT_AUDIT_LOGGER_HPP

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __cplusplus
#include "systemTimeValidator.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#define ENABLE_LOGGING 1

#define DATE_BUFFER_SIZE    13
#define TIME_BUFFER_SIZE    10
#define TAG_BUFFER_SIZE     16
#define MESSAGE_BUFFER_SIZE 256
#define PATH_BUFFER_SIZE    256

#define LOGS_DIR                 "logs"
#define SYSTEM_LOG               "logs/system.log"
#define CONNECTIONS_LOG          "logs/connections.log"
#define REGISTER_CONNECTIONS_LOG "logs/connections.json"

#define INFO_TAG                "[    Info    ]"
#define ERROR_TAG               "[    Error   ]"
#define REGISTER_TAG            "[ Connection ]"
#define EVENT_TAG               "[    Event   ]"
#define CONTROL_TAG             "[   Control  ]"
#define WARNING_TAG             "[   Warning  ]"
#define CRITICAL_TAG            "[  Critical  ]"
#define DEBUG_TAG               "[    Debug   ]"
#define DEFAULT_TAG             "[    Log     ]"
#define DEFAULT_DIR_PERMISSIONS 0755

    /**
     * @brief Enum indicating the severity or type of log entry.
     */
    typedef enum
    {
        INFO_LOG,
        CONTROL_LOG,
        ERROR_LOG,
        WARNING_LOG,
        CRITICAL_LOG,
        EVENT_LOG,
        DEBUG_LOG,
        CONNECTION_LOG,
    } LogType;

#ifdef __cplusplus
}
#endif

#ifndef __cplusplus
typedef struct
{
    char entryDate[DATE_BUFFER_SIZE];
    char separator1;
    char entryTime[TIME_BUFFER_SIZE];
    char separator2;
    char tag[TAG_BUFFER_SIZE];
    char separator3;
    char message[MESSAGE_BUFFER_SIZE];
} LogEntry;
#else
struct LogEntry
{
    char entryDate[DATE_BUFFER_SIZE];
    char separator1;
    char entryTime[TIME_BUFFER_SIZE];
    char separator2;
    char tag[TAG_BUFFER_SIZE];
    char separator3;
    char message[MESSAGE_BUFFER_SIZE];
};
#endif

/**
 * @brief Class responsible for managing system logs, providing access to processes for auditing.
 */
#ifdef __cplusplus
class EventAuditLogger
{
private:
    char logFilePath[256];
    LogEntry currentEntry;
    std::mutex loggerMutex;

public:
    EventAuditLogger();
    int writeLog(const char* logMessage, LogType type);
    void writeString(const std::string& message, LogType type)
    {
        writeLog(message.c_str(), type);
    }
    void setLogFile(const char* filePath)
    {
        std::strncpy(logFilePath, filePath, sizeof(logFilePath) - 1);
        logFilePath[sizeof(logFilePath) - 1] = '\0';
    }

private:
    int writeToJSON(const char* logMessage);
    int writeToSpecificLog(const char* filePath);
};

inline EventAuditLogger auditLogger;
#endif

#ifdef __cplusplus
extern "C"
{
#endif
    int audit_log_write(const char* logMessage, LogType type);
    int audit_log_write_fmt(LogType type, const char* format, ...);
#ifdef __cplusplus
}
#endif

#endif
