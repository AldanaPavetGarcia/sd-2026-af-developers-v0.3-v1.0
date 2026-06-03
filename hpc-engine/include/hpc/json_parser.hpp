#ifndef HPC_JSON_PARSER_HPP
#define HPC_JSON_PARSER_HPP

/**
 * @file json_parser.hpp
 * @brief JSON ingestion layer: parses ANALYZE_GRAPH requests into internal structures.
 */

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "graph.hpp"

namespace hpc
{

/**
 * @brief Holds the parsed content of an ANALYZE_GRAPH request.
 */
struct AnalyzeRequest
{
    std::string request_id;
    std::string traceparent;
    std::string tracestate;
    Graph graph;
    std::vector<std::string> algorithms;
};

/**
 * @brief Result of a JSON parse attempt.
 */
struct ParseResult
{
    bool ok = false;
    AnalyzeRequest request;
    std::string error_code;
    std::string error_message;
    std::string error_field;
};

/**
 * @brief Parse a raw JSON string into an AnalyzeRequest.
 *
 * Performs structural parsing only (JSON well-formedness and required fields).
 * Graph validation (semantic checks) is handled by the Validator module.
 *
 * @param raw_json The raw JSON string from stdin.
 * @return ParseResult with ok=true on success, or error details on failure.
 */
ParseResult parseRequest(const std::string& raw_json);

} // namespace hpc

#endif // HPC_JSON_PARSER_HPP
