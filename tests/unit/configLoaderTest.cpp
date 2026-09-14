#include "../../lib/include/configLoader.hpp"
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>

TEST(ConfigLoaderTest, LoadValidFile)
{
    // Crear un archivo temporal de config
    const char* testFile = "test_config.env";
    std::ofstream out(testFile);
    out << "KEY1=value1\n";
    out << "# Comentario\n";
    out << "PORT=1234\n";
    out << "\n";
    out << "TIMEOUT=30\n";
    out.close();

    ConfigLoader loader(testFile);
    EXPECT_EQ(loader.getString("KEY1"), "value1");
    EXPECT_EQ(loader.getInt("PORT"), 1234);
    EXPECT_EQ(loader.getInt("TIMEOUT"), 30);

    // Valores por defecto
    EXPECT_EQ(loader.getString("NONEXISTENT", "def"), "def");
    EXPECT_EQ(loader.getInt("NONEXISTENT", 99), 99);

    std::remove(testFile);
}

TEST(ConfigLoaderTest, InvalidInteger)
{
    const char* testFile = "test_invalid.env";
    std::ofstream out(testFile);
    out << "NOT_INT=abc\n";
    out.close();

    ConfigLoader loader(testFile);
    EXPECT_EQ(loader.getInt("NOT_INT", 55), 55);

    std::remove(testFile);
}

TEST(ConfigLoaderTest, MissingFile)
{
    ConfigLoader loader("nonexistent_file.env");
    EXPECT_EQ(loader.getString("ANY", "def"), "def");
    EXPECT_EQ(loader.getInt("ANY", 10), 10);
}

// A key present but holding something that is not a number is a different case from a key that
// is absent: the absent path returns the default without ever parsing, while this one goes
// through std::stoi and has to survive the exception it throws.
TEST(ConfigLoaderTest, NonNumericValueFallsBackToTheDefaultInsteadOfThrowing)
{
    const char* testFile = "test_config_bad_int.env";
    std::ofstream out(testFile);
    out << "PORT=not-a-number\n";
    out << "EMPTY=\n";
    out.close();

    ConfigLoader loader(testFile);

    EXPECT_EQ(loader.getInt("PORT", 8080), 8080);
    EXPECT_EQ(loader.getInt("EMPTY", 42), 42);

    // The raw string is still readable -- only the integer conversion falls back.
    EXPECT_EQ(loader.getString("PORT"), "not-a-number");

    std::remove(testFile);
}
