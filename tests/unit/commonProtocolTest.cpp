#include "../../lib/include/commonProtocol.hpp"
#include <gtest/gtest.h>

TEST(CommonProtocolTest, GetCmdStringFromCode)
{
    EXPECT_EQ(getCmdStringFromCode(CMD_REGISTER), "REGISTER");
    EXPECT_EQ(getCmdStringFromCode(CMD_ACK), "ACK");
    EXPECT_EQ(getCmdStringFromCode(CMD_ERROR_DUPLICATE), "ERROR_DUPLICATE");
    EXPECT_EQ(getCmdStringFromCode(CMD_QUERY_NODE), "QUERY_NODE");
    EXPECT_EQ(getCmdStringFromCode(CMD_ERROR_NOT_FOUND), "ERROR_NOT_FOUND");
    EXPECT_EQ(getCmdStringFromCode(CMD_LIST_NODES), "LIST_NODES");
    EXPECT_EQ(getCmdStringFromCode(CMD_ANALYZE_GRAPH), "ANALYZE_GRAPH");
    EXPECT_EQ(getCmdStringFromCode(CMD_ERROR_SERVICE_UNAVAILABLE), "ERROR_SERVICE_UNAVAILABLE");
    EXPECT_EQ(getCmdStringFromCode(static_cast<SystemCommand>(999)), "UNKNOWN"); // NOLINT
}

TEST(CommonProtocolTest, GetStatusString)
{
    EXPECT_EQ(getStatusString(OFFLINE_STATUS), "OFFLINE");
    EXPECT_EQ(getStatusString(ONLINE_STATUS), "ONLINE");
    EXPECT_EQ(getStatusString(static_cast<ProcessStatus>(999)), "UNKNOWN"); // NOLINT
}

TEST(CommonProtocolTest, RandomNumberGeneration)
{
    seedRandomNumber();
    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        int r = getRandomNumber();
        EXPECT_GE(r, 1);
        EXPECT_LE(r, MAX_CLIENTS);
    }
}
