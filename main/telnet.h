/*
 * Telnet Protocol (RFC 854 + RFC 856) — minimal server for serial bridge
 *
 * Negotiates Binary Transmission mode on connect so that after a brief
 * handshake the connection becomes a transparent byte pipe, identical
 * to raw TCP. Standard telnet clients can then be used without any
 * special raw-mode flags.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize a new Telnet session for the given socket.
 * Sends the initial IAC option negotiation sequence:
 *   IAC DO BINARY   — ask client to use binary transmission
 *   IAC WILL BINARY — announce we will use binary transmission
 *   IAC WILL ECHO   — claim we echo (so client disables local echo)
 *   IAC WILL SGA    — suppress go-ahead
 */
void telnet_init_session(int sock);

/*
 * Process one byte received from the Telnet client.
 *
 * Returns true if the byte should be forwarded to UART.
 * Returns false if the byte was consumed as Telnet protocol data
 * (IAC command, option negotiation, subnegotiation).
 *
 * Once telnet_is_binary_mode() returns true, callers should bypass
 * this function entirely and forward all bytes directly to UART.
 */
bool telnet_process_rx_byte(uint8_t byte);

/*
 * Returns true once BINARY mode is fully negotiated in both directions.
 * When true, all data can pass through transparently without filtering.
 */
bool telnet_is_binary_mode(void);

/*
 * Returns true if the negotiation timeout has been exceeded without
 * binary mode being established. The caller should close the connection
 * in this case — the client is either too old or misbehaving.
 */
bool telnet_negotiation_timed_out(void);

/*
 * Reset internal state for a new connection.
 */
void telnet_reset(void);

#ifdef __cplusplus
}
#endif
