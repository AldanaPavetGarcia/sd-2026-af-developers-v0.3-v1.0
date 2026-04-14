#include "../../lib/include/eventAuditLogger.hpp"
#include <gtest/gtest.h>
#include <unistd.h>

TEST(EventAuditLoggerTest, WriteLogTypes)
{
    EventAuditLogger logger;
    logger.setLogFile("/tmp/test_system.log");

    EXPECT_GT(logger.writeLog("Test INFO", INFO_LOG), 0);
    EXPECT_GT(logger.writeLog("Test CONTROL", CONTROL_LOG), 0);
    EXPECT_GT(logger.writeLog("Test ERROR", ERROR_LOG), 0);
    EXPECT_GT(logger.writeLog("Test WARNING", WARNING_LOG), 0);
    EXPECT_GT(logger.writeLog("Test CRITICAL", CRITICAL_LOG), 0);
    EXPECT_GT(logger.writeLog("Test EVENT", EVENT_LOG), 0);
    EXPECT_GT(logger.writeLog("Test DEBUG", DEBUG_LOG), 0);
    EXPECT_GT(logger.writeLog("Test DEFAULT", (LogType)999), 0); // NOLINT

    // CONNECTION_LOG should also trigger writeToJSON
    EXPECT_GT(logger.writeLog("Test CONNECTION", CONNECTION_LOG), 0);

    // C API wrappers
    EXPECT_GT(audit_log_write("C API Test", INFO_LOG), 0);
    EXPECT_GT(audit_log_write_fmt(INFO_LOG, "C API %s", "Formatted"), 0);

    unlink("/tmp/test_system.log");
    unlink(REGISTER_CONNECTIONS_LOG);
}
