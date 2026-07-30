/**
 * @file main.cpp
 * @brief HPC Engine entry point: reads JSON from stdin, processes graph, writes result to stdout.
 *
 * Exit codes:
 *   0 — Success (ACK response written to stdout)
 *   1 — Validation or parse error (ERROR response written to stdout)
 *   2 — Unexpected internal error
 */

#include <iostream>
#include <sstream>
#include <string>

#include "hpc/engine.hpp"

int main()
{
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();

    const hpc::EngineProcessResult result = hpc::processAnalyzeGraphRequest(buffer.str());

    std::cout << result.payload;
    return result.exit_code;
}
