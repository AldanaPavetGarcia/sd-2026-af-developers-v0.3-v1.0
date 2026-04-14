#include "../include/configLoader.hpp"
#include <fstream>
#include <iostream>
#include <sstream>

ConfigLoader::ConfigLoader(const std::string& filePath)
{
    loadFromFile(filePath);
}

void ConfigLoader::loadFromFile(const std::string& filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open())
    {
        return;
    }

    std::string line;
    while (std::getline(file, line))
    {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        auto pos = line.find('=');
        if (pos != std::string::npos)
        {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            configMap[key] = value;
        }
    }
}

std::string ConfigLoader::getString(const std::string& key, const std::string& defaultValue) const
{
    auto it = configMap.find(key);
    if (it != configMap.end())
    {
        return it->second;
    }
    return defaultValue;
}

int ConfigLoader::getInt(const std::string& key, int defaultValue) const
{
    auto it = configMap.find(key);
    if (it != configMap.end())
    {
        try
        {
            return std::stoi(it->second);
        }
        catch (...)
        {
            return defaultValue;
        }
    }
    return defaultValue;
}
