/**
 * @file  telnet.h
 * @brief Telnet Protocol (RFC 854 + RFC 856) — minimal server for serial bridge
 *
 * Negotiates Binary Transmission mode on connect so that after a brief
 * handshake the connection becomes a transparent byte pipe, identical
 * to raw TCP.  Standard telnet clients can then be used without any
 * special raw-mode flags.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize a new Telnet session for the given socket.
 *
 * Sends the initial IAC option negotiation sequence:
 *   - IAC DO BINARY   — ask client to use binary transmission
 *   - IAC WILL BINARY — announce we will use binary transmission
 *   - IAC WILL ECHO   — claim we echo (so client disables local echo)
 *   - IAC WILL SGA    — suppress go-ahead
 *
 * @param sock  TCP socket for the new Telnet session
 */
void telnet_init_session(int sock);

/**
 * @brief Process one byte received from the Telnet client.
 *
 * @param byte  A single byte from the TCP stream.
 * @retval true   Byte should be forwarded to UART as regular data.
 * @retval false  Byte was consumed as Telnet protocol data
 *                (IAC command, option negotiation, subnegotiation).
 *
 * @note Once telnet_is_binary_mode() returns true, callers should bypass
 *       this function entirely and forward all bytes directly to UART.
 */
bool telnet_process_rx_byte(uint8_t byte);

/**
 * @brief Returns true once BINARY mode is fully negotiated in both directions.
 *
 * When true, all data can pass through transparently without filtering.
 */
bool telnet_is_binary_mode(void);

/**
 * @brief Returns true if the negotiation timeout has been exceeded.
 *
 * Indicates that binary mode was not established within the configured
 * timeout.  The caller should close the connection — the client is
 * either too old or misbehaving.
 */
bool telnet_negotiation_timed_out(void);

/** @brief Reset internal state for a new connection. */
void telnet_reset(void);

#ifdef __cplusplus
}
#endif
