#include "../include/systemTimeValidator.hpp"

void Time::captureTimeToString(std::span<char> buffer, bool includeSeconds)
{
    time_t now = time(nullptr);
    struct tm* timeInfo = localtime(&now);
    if (timeInfo != nullptr)
    {
        if (includeSeconds)
        {
            std::strftime(buffer.data(), buffer.size(), "%H:%M:%S", timeInfo);
        }
        else
        {
            std::strftime(buffer.data(), buffer.size(), "%H:%M", timeInfo);
        }
    }
}

void Date::captureDateToString(std::span<char> buffer, bool isY2K)
{
    time_t now = time(nullptr);
    struct tm* timeInfo = localtime(&now);
    if (timeInfo != nullptr)
    {
        if (isY2K)
        {
            std::strftime(buffer.data(), buffer.size(), "%d/%m/%Y", timeInfo);
        }
        else
        {
            std::strftime(buffer.data(), buffer.size(), "%d/%m/%y", timeInfo);
        }
    }
}

// Dummy implementations
long Time::compare(int h[3])
{
    (void)h;
    return 0;
}
void Time::captureSystemTime() {}
int Time::getHour()
{
    return 0;
}
void Time::formatTimeToString(std::span<char> buffer, bool includeSeconds)
{
    (void)buffer;
    (void)includeSeconds;
}

void Date::captureSystemDate() {}
void Date::formatDateToString(std::span<char> buffer, bool isY2K, bool isInverted)
{
    (void)buffer;
    (void)isY2K;
    (void)isInverted;
}
void Date::captureDateToStringInverted(std::span<char> buffer, bool isY2K)
{
    (void)buffer;
    (void)isY2K;
}

DateTimeConfig::DateTimeConfig() {}
