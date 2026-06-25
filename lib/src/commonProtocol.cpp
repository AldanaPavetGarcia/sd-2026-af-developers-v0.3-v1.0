#include "../include/commonProtocol.hpp"
#include <signal.h>

// Definition of global variables declared in header.
RngType rng;
std::atomic<bool> isTerminationSignalReceived {false};

/**
 * @brief SIGTERM signal reception handler.
 *
 */
// LCOV_EXCL_START
void sigtermHandler(int signum, siginfo_t* info, void* ptr)
{
    (void)signum;
    (void)info;
    (void)ptr;
    isTerminationSignalReceived = true;
}
// LCOV_EXCL_STOP

/**
 * @brief Registers process signal handlers for graceful shutdown.
 *
 * SIGTERM is sent by Docker and Kubernetes when stopping the container.
 * SIGINT is registered as well to support local Ctrl+C execution.
 */
// LCOV_EXCL_START
void setSigaction(void)
{
    struct sigaction action = {};
    action.sa_sigaction = sigtermHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;

    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}
// LCOV_EXCL_STOP

/**
 * @brief Returns a string with the name of the command indicated by code.
 *
 * @param code command number
 * @return String with the name of the command
 */
std::string getCmdStringFromCode(SystemCommand code)
{
    std::string cmdStr;

    switch (code)
    {
        case CMD_REGISTER: cmdStr = "REGISTER"; break;
        case CMD_ACK: cmdStr = "ACK"; break;
        case CMD_ERROR_DUPLICATE: cmdStr = "ERROR_DUPLICATE"; break;
        case CMD_QUERY_NODE: cmdStr = "QUERY_NODE"; break;
        case CMD_ERROR_NOT_FOUND: cmdStr = "ERROR_NOT_FOUND"; break;
        case CMD_LIST_NODES: cmdStr = "LIST_NODES"; break;
        case CMD_HEARTBEAT: return "HEARTBEAT";
        case CMD_ANALYZE_GRAPH: cmdStr = "ANALYZE_GRAPH"; break;
        case CMD_ERROR_SERVICE_UNAVAILABLE: cmdStr = "ERROR_SERVICE_UNAVAILABLE"; break;
        case CMD_ERROR_HPC_TIMEOUT: cmdStr = "ERROR_HPC_TIMEOUT"; break;
        default: cmdStr = "UNKNOWN"; break;
    }

    return cmdStr;
}

/**
 * @brief Returns a string matching the status passed by parameter, for logging purposes.
 *
 * @param statusCode status number
 * @return string containing the status name
 */
std::string getStatusString(ProcessStatus statusCode)
{
    std::string result;

    switch (statusCode)
    {
        case OFFLINE_STATUS: result = "OFFLINE"; break;
        case ONLINE_STATUS: result = "ONLINE"; break;
        default: result = "UNKNOWN"; break;
    }

    return result;
}

/**
 * @brief Initializes the random number generator.
 *
 */
void seedRandomNumber(void)
{
    auto const seedval = static_cast<RngType::result_type>(std::time(nullptr));
    rng.seed(seedval);
}

/**
 * @brief Returns a random number between 1 and MAX_CLIENTS (inclusive).
 *
 * @return int random number
 */
int getRandomNumber(void)
{
    std::uniform_int_distribution<int> udist(1, MAX_CLIENTS);
    return udist(rng);
}