#pragma once

#include <string>

namespace hpc
{

    struct EngineProcessResult
    {
        std::string payload;
        int exit_code;
    };

    /**
     * @brief Processes one ANALYZE_GRAPH JSON request and returns the serialized response.
     *
     * This function contains the reusable HPC Engine execution path:
     * parse request, validate graph, execute algorithms, and build the JSON response.
     *
     * @param raw_input Raw JSON request payload.
     * @return Serialized response payload and process-compatible exit code.
     */
    EngineProcessResult processAnalyzeGraphRequest(const std::string& raw_input);

} // namespace hpc
