#ifndef SYSTEM_TIME_VALIDATOR_HPP
#define SYSTEM_TIME_VALIDATOR_HPP

#include <cstdio>
#include <ctime>
#include <iostream>
#include <span>
#include <stdint.h>
#include <sys/time.h>

class Time
{
private:
    int hour [[maybe_unused]];
    int minutes [[maybe_unused]];
    int seconds [[maybe_unused]];
    time_t julianDate [[maybe_unused]];

public:
    long compare(int h[3]);
    void captureSystemTime();
    int getHour();
    void formatTimeToString(std::span<char> buffer, bool includeSeconds);
    void captureTimeToString(std::span<char> buffer, bool includeSeconds);
    Time()
        : hour(0)
        , minutes(0)
        , seconds(0)
        , julianDate(0)
    {
    }
};

/*!
 * @brief Class responsible for providing access to the current date.
 */
class Date
{
private:
    int day [[maybe_unused]];
    int month [[maybe_unused]];
    int year [[maybe_unused]];
    int julian [[maybe_unused]];

public:
    void captureSystemDate();
    void formatDateToString(std::span<char> buffer, bool isY2K, bool isInverted);
    void captureDateToString(std::span<char> buffer, bool isY2K);
    void captureDateToStringInverted(std::span<char> buffer, bool isY2K);
};

/*!
 * @brief Class responsible for configuring system date and time.
 */
class DateTimeConfig
{
private:
    Date systemDate [[maybe_unused]];
    Time systemTime [[maybe_unused]];

public:
    DateTimeConfig();
};

uint64_t wdogGetTimeMs();

#endif
