#include "../../lib/include/systemTimeValidator.hpp"
#include <array>
#include <gtest/gtest.h>

TEST(SystemTimeValidatorTest, TimeCapture)
{
    Time t;
    std::array<char, 20> buf;
    t.captureTimeToString(buf, true);
    EXPECT_GT(std::string(buf.data()).length(), 0);

    t.captureTimeToString(buf, false);
    EXPECT_GT(std::string(buf.data()).length(), 0);
}

TEST(SystemTimeValidatorTest, DateCapture)
{
    Date d;
    std::array<char, 20> buf;
    d.captureDateToString(buf, true);
    EXPECT_GT(std::string(buf.data()).length(), 0);

    d.captureDateToString(buf, false);
    EXPECT_GT(std::string(buf.data()).length(), 0);
}

TEST(SystemTimeValidatorTest, DummyFunctionsCoverage)
{
    Time t;
    int h[3] = {1, 2, 3};
    EXPECT_EQ(t.compare(h), 0);
    t.captureSystemTime();
    EXPECT_EQ(t.getHour(), 0);

    std::array<char, 20> buf;
    t.formatTimeToString(buf, true);

    Date d;
    d.captureSystemDate();
    d.formatDateToString(buf, true, true);
    d.captureDateToStringInverted(buf, true);

    DateTimeConfig cfg;
}
