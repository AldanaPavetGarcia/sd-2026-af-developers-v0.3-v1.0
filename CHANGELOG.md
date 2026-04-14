# Changelog

All notable changes to the EOP Server Node Registry project will be documented in this file.

## [Unreleased] - 2026-04-07

### Added
- **Merged Welcome Message (ACK):** The server now responds to a successful `REGISTER` command with a unified greeting/ACK string formatted as `ACK Server v1.0 | TIME <timestamp> | SESSION_ID <node_id>`.

### Changed
- **Direct Node Registration:** Replaced the legacy two-step session process. The initialization of a session directly relies on parsing a `REGISTER` command from an accepted connection.
- **Node ID Decoupling:** Re-adapted the connection limit verification. Allowed connection count (`MAX_CLIENTS=10`) is exclusively checked against the global active connections tracker instead of bounding numerical ranges from `node_id`.
- **Integration Test Overhaul:** Updated the `testConnection.cpp` `Reaches100ConnectionsSuccessfully` test case to perform the 100-cycle loop (Connect → Register → Acknowledge → Disconnect) utilizing the standard unified protocol, dropping its dependency on deprecated session allocations.

### Removed
- **Legacy REQ_SESSION System:** Extracted and eliminated the legacy `isLegacySession` logic, `REQ_SESSION` command parsing, and numeric `activeSessions` indexing structures.

### Fixed
- **Static Analysis & Linting Warnings (`clang-tidy`):**
  - Included a strict prototype `(void)` definition on `watchdog_get_current_time_ms` in `timed_watchdog_controller.c` to fix warnings natively triggered under standard C flags.
  - Added socket connection bounds checking (`ASSERT_GE(sock, 0)`) in test files to resolve unvalidated `-1` descriptor assertions flagged by UNIX library analyzers.
- **Code Styling Constraints:** Formatted the entire core codebase (`.cpp`, `.hpp`, `.h`) automatically conforming flawlessly to `clang-format-18` style guides.
- **EOF Protocol Warnings:** Fixed ending empty-line parsing problems inside `commonProtocol.hpp`.
