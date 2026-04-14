#ifndef CONFIG_LOADER_HPP
#define CONFIG_LOADER_HPP

#include <string>
#include <unordered_map>

/**
 * @brief Simple configuration loader that reads key=value pairs from a file.
 */
class ConfigLoader
{
public:
    /**
     * @brief Constructor.
     * @param filePath Path to the .env or config file.
     */
    explicit ConfigLoader(const std::string& filePath);

    /**
     * @brief Get a value as string.
     * @param key The key to look for.
     * @param defaultValue Value to return if key is not found.
     * @return The value from config or defaultValue.
     */
    std::string getString(const std::string& key, const std::string& defaultValue = "") const;

    /**
     * @brief Get a value as integer.
     * @param key The key to look for.
     * @param defaultValue Value to return if key is not found or invalid.
     * @return The value from config or defaultValue.
     */
    int getInt(const std::string& key, int defaultValue = 0) const;

private:
    std::unordered_map<std::string, std::string> configMap;
    void loadFromFile(const std::string& filePath);
};

#endif
