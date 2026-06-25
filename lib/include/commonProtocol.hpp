#ifndef COMMON_PROTOCOL_HPP
#define COMMON_PROTOCOL_HPP

#ifdef __cplusplus
inline constexpr int MAX_CLIENTS = 10;
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unordered_map>

typedef std::mt19937 RngType;
extern RngType rng;
extern std::atomic<bool> isTerminationSignalReceived;

// ---------------------------------------------------------------------------
// Node-registry protocol commands
// ---------------------------------------------------------------------------

extern "C"
{
#else
#define MAX_CLIENTS 10
#include <stdbool.h>
#include <stdint.h>
#endif

    /**
     * @brief Enum for node registration commands.
     */
    enum SystemCommand
    {
        CMD_REGISTER,
        CMD_ACK,
        CMD_ERROR_DUPLICATE,
        CMD_QUERY_NODE,
        CMD_ERROR_NOT_FOUND,
        CMD_LIST_NODES,
        CMD_HEARTBEAT,
        CMD_ANALYZE_GRAPH,
        CMD_ERROR_SERVICE_UNAVAILABLE,
    };

    /**
     * @brief Enum for various process states.
     */
    enum ProcessStatus
    {
        OFFLINE_STATUS,
        ONLINE_STATUS
    };

    // Global flags
#ifndef __cplusplus
    extern bool isTerminationSignalReceived;
#endif

    void modifyStringBuffer(char* str, int a, int b);
    bool pipeExists(const char* pipeName);
    void ltrim(char* src, char* dst, long len);
    long parseStringToLong(char* str, long defaultValue);
    void setSigaction(void);
    void restartConnectionController(void);
    void rebootOs(void);
    void shutdownOs(void);
    void seedRandomNumber(void);
    int getRandomNumber(void);

#ifdef __cplusplus
} // end extern "C"

std::string getCmdStringFromCode(SystemCommand code);
std::string getStatusString(ProcessStatus statusCode);
#endif

#endif
