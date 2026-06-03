# ADR 003: Messaging Protocol Design

## Context and Problem

For the EOP system to function as a cohesive product and not as isolated projects, it is imperative that all services (C++ server and C clients) use a common "language." The protocol must unambiguously define message boundaries (framing), identify the types of operations, and allow for system evolution without breaking compatibility. The accuracy of this design is critical, as it must allow an implementation by Team A to be fully interoperable with that of Team B without needing to consult their source code.

## Options Considered

### **Payload Encoding**

1. **Custom Binary:** Compact and fast to process, but difficult to debug and requires an extremely rigid specification of byte order (endianness).

2. **Protobuf:** Offers mandatory schemas and efficiency, but adds a complex external dependency (`protoc`) that can complicate compilation in the initial stages.

3. **JSON (JavaScript Object Notation):** A widely supported and flexible text-based format.

### **Framing**

1. **Length-prefix:** A fixed-size header (e.g., 4 bytes) indicates the message length. It is deterministic and supports binary data, but requires multiple calls to `recv()`.

2. **Delimiter:** Uses a special character (e.g., `\n`) to mark the end of the message. It is very simple to implement for text-based protocols.

## Decision

It has been decided to implement a **JSON**-based protocol for payload encoding, using a **Delimiter (`\n`)** for message framing.

## Justification (Trade-off Analysis)

* **Observability and Debugging:** Choosing **JSON** makes messages human-readable. This greatly facilitates diagnostics using tools like `netcat`, `telnet`, or `Wireshark` during development and load testing, which is vital for validating acceptance criteria (AC).

* **Ease of Implementation:** Using a delimiter like the newline character (`\n`) simplifies the extraction logic in the server's main loop, allowing for seamless integration with C++ string manipulation functions.

* **Flexibility:** JSON allows for the addition of new optional fields in future versions (v0.2 or v0.3) without breaking compatibility for clients that don't expect them, facilitating protocol evolution without constant renegotiations.

* * **Interoperability:** As a universal standard, it guarantees that components written by different teams can communicate as long as they adhere to the agreed-upon field scheme (Envelope).

## Consequences

* **Positive:** Reduced debugging time; clear and accessible protocol documentation; ease of extending the system with new message types such as `ANALYZE_GRAPH`.

* **Negative:** Higher processing overhead compared to a binary format, due to text parsing. Higher bandwidth consumption per message.

* **Evolution Rules:**

* **Freezing:** Once accepted, the protocol is considered "frozen." Incompatible changes (removing or renaming fields) require a new version and collective renegotiation.

* **Validation:** Interoperability will be verified through cross-testing where the client on one team must operate correctly with the server on another.

* * **Security:** Engineers are prohibited from including sensitive data (tokens, passwords) in JSON fields, thus maintaining secure telemetry output.

## v0.2 Addendum: ANALYZE_GRAPH Protocol Extension

For Milestone 2, the EOP protocol is extended with a new message type: `ANALYZE_GRAPH`.

This extension preserves the existing protocol decisions:

- Payload encoding remains JSON.
- Message framing remains delimiter-based using `\n`.
- Existing v0.1 message types remain unchanged.
- The extension is additive and non-breaking.

The `ANALYZE_GRAPH` message allows a C client to request graph analysis through the existing EOP server. The server dispatches the request to the registered HPC engine and returns the result to the originating client.

The detailed request schema, response schema, validation rules, error codes, and sequence diagram are documented in:

`docs/analyze-graph-protocol-v0.2.md`
--