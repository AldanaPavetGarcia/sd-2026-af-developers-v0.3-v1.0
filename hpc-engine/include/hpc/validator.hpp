#ifndef HPC_VALIDATOR_HPP
#define HPC_VALIDATOR_HPP

/**
 * @file validator.hpp
 * @brief Graph validation layer — verifies topology integrity before processing.
 *
 * Implements strict validation per US-202 acceptance criteria:
 * - Non-empty nodes list
 * - Unique node identifiers
 * - Edge references point to existing nodes
 * - Edge weights are positive finite numbers
 * - Requested algorithms are supported
 */

#include <string>

#include "graph.hpp"

namespace hpc
{

/**
 * @brief Result of a graph validation check.
 */
struct ValidationResult
{
    bool ok = true;
    std::string error_code;
    std::string error_message;
    std::string error_field;
};

/**
 * @brief Supported algorithm names.
 */
inline const std::vector<std::string> SUPPORTED_ALGORITHMS = {
    "shortest_paths",
    "connected_components",
    "centrality"};

/**
 * @brief Validate a parsed graph and algorithm list.
 *
 * Must be called after parseRequest() and before running any algorithm.
 * On failure, returns a ValidationResult with structured error details
 * suitable for building an ERROR response.
 *
 * @param graph The graph to validate.
 * @param algorithms The list of requested algorithm names.
 * @return ValidationResult with ok=true if valid, or error details.
 */
ValidationResult validateGraph(const Graph& graph, const std::vector<std::string>& algorithms);

} // namespace hpc

#endif // HPC_VALIDATOR_HPP
