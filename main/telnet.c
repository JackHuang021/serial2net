/*
 * Telnet Protocol (RFC 854 + RFC 856 Binary Transmission)
 *
 * Minimal server implementation for serial bridge use.
 * On connect, immediately negotiates binary mode so that after a brief
 * handshake the TCP stream becomes a transparent byte pipe — identical
 * to raw TCP but accessible via standard `telnet` clients.
 */

#include "telnet.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TELNET_TAG = "telnet";

/* ---- Telnet protocol constants (RFC 854) ---- */
#define TELNET_IAC    255   /* Interpret As Command                    */
#define TELNET_WILL   251   /* Sender wants to enable option           */
#define TELNET_WONT   252   /* Sender wants to disable option          */
#define TELNET_DO     253   /* Sender wants peer to enable option      */
#define TELNET_DONT   254   /* Sender wants peer to disable option     */
#define TELNET_SB     250   /* Subnegotiation begin                    */
#define TELNET_SE     240   /* Subnegotiation end                      */

/* ---- Telnet option codes ---- */
#define TELNET_OPT_BINARY      0   /* RFC 856: Binary Transmission     */
#define TELNET_OPT_ECHO        1   /* RFC 857: Echo                    */
#define TELNET_OPT_SGA         3   /* RFC 858: Suppress Go Ahead       */

/* ---- State machine ---- */
typedef enum {
    TELNET_STATE_DATA,       /* Normal byte passthrough                */
    TELNET_STATE_IAC,        /* Received 0xFF, awaiting command byte   */
    TELNET_STATE_OPTION,     /* Received WILL/WONT/DO/DONT, await opt  */
    TELNET_STATE_SUBNEG,     /* Inside subnegotiation (SB ... IAC SE)  */
    TELNET_STATE_SUBNEG_IAC, /* Got IAC inside subnegotiation          */
} telnet_state_t;

/* ---- Module state ---- */
static int             tn_sock = -1;
static telnet_state_t  tn_state = TELNET_STATE_DATA;
static bool            tn_binary_rx = false;   /* Client → Server binary */
static bool            tn_binary_tx = false;   /* Server → Client binary */
static TickType_t      tn_connect_ticks = 0;

/* ---- Command byte being processed in OPTION state ---- */
static uint8_t         tn_pending_cmd = 0;

/* ================================================================
 *  Internal helpers
 * ================================================================ */

static void tn_send_triple(uint8_t cmd, uint8_t opt)
{
    uint8_t seq[3] = { TELNET_IAC, cmd, opt };
    send(tn_sock, seq, sizeof(seq), 0);
}

/* Respond to a peer's WILL/WONT/DO/DONT for a given option.
 *
 * IMPORTANT: Once binary mode is fully negotiated (both tn_binary_rx
 * and tn_binary_tx are set), this function suppresses ALL further IAC
 * responses.  Sending IAC triples after the peer has entered binary RX
 * mode causes them to be treated as raw data — they leak into the
 * display as garbage characters. */
static void tn_handle_command(uint8_t cmd, uint8_t opt)
{
    bool already_binary = tn_binary_rx && tn_binary_tx;

    switch (opt) {

    case TELNET_OPT_BINARY:
        /* The initial burst (telnet_init_session) already sent
         * DO BINARY and WILL BINARY — the client's DO/WILL BINARY
         * responses are acknowledgments that do not need a reply.
         *
         * Sending further IAC triples here (the "BINARY ACK") would
         * be a second TCP segment that arrives *after* the client has
         * processed our WILL BINARY and entered binary RX mode — the
         * ACK bytes would leak as raw data onto the user's terminal. */
        if (cmd == TELNET_WILL) {
            tn_binary_rx = true;   /* Peer will send in binary */
        } else if (cmd == TELNET_DO) {
            tn_binary_tx = true;   /* We may send in binary    */
        }
        if (tn_binary_rx && tn_binary_tx && !already_binary) {
            ESP_LOGI(TELNET_TAG, "Binary mode negotiated — transparent bridge active");
        }
        break;

    case TELNET_OPT_ECHO:
        /* We already sent WILL ECHO in the initial burst — the client
         * knows we claim to echo and has disabled local echo.  Any
         * further ECHO negotiations from the client are silently
         * ignored: sending DONT / WONT replies would generate IAC
         * traffic that can cross with the binary-mode transition
         * and leak as garbage characters on the terminal. */
        break;

    case TELNET_OPT_SGA:
        /* Same reasoning as ECHO above — our initial WILL SGA already
         * told the client everything it needs.  No further responses. */
        break;

    default:
        /* Silently ignore all unknown options — never reject with
         * DONT / WONT because the reply IAC triple could cross with
         * the binary-mode transition and leak as raw data. The peer
         * will eventually stop retrying. */
        break;
    }
}

/* ================================================================
 *  Public API
 * ================================================================ */

void telnet_init_session(int sock)
{
    tn_sock           = sock;
    tn_state          = TELNET_STATE_DATA;
    tn_binary_rx      = false;
    tn_binary_tx      = false;
    tn_connect_ticks  = xTaskGetTickCount();

    /* Kick off option negotiation immediately.
     * Most clients respond within a single round-trip.
     *
     * ORDER IS CRITICAL: WILL BINARY must be the LAST triple sent.
     * When the client processes IAC WILL BINARY, it enters binary RX
     * mode and stops interpreting IAC in received data — any IAC
     * triples that follow would be treated as raw binary data and
     * appear as garbage characters on the terminal. */
    tn_send_triple(TELNET_WILL, TELNET_OPT_ECHO);    /* We (pretend to) echo */
    tn_send_triple(TELNET_WILL, TELNET_OPT_SGA);     /* Suppress Go Ahead   */
    tn_send_triple(TELNET_DO,   TELNET_OPT_BINARY);  /* Please use binary   */
    tn_send_triple(TELNET_WILL, TELNET_OPT_BINARY);  /* We will use binary  (MUST BE LAST) */
}

bool telnet_process_rx_byte(uint8_t byte)
{
    /* This function is called for EVERY byte received from a Telnet
     * client, both during and after negotiation.  It filters out IAC
     * command sequences (which would otherwise leak to UART and be
     * echoed back as garbage) while passing through regular data
     * bytes including literal 0xFF (sent by the client as IAC-IAC). */
    switch (tn_state) {

    case TELNET_STATE_DATA:
        if (byte == TELNET_IAC) {
            tn_state = TELNET_STATE_IAC;
            return false;          /* IAC consumed — not UART data    */
        }
        return true;               /* Normal byte — forward to UART   */

    case TELNET_STATE_IAC:
        if (byte == TELNET_IAC) {
            /* IAC IAC → literal 0xFF data byte */
            tn_state = TELNET_STATE_DATA;
            return true;           /* Emit the literal 0xFF            */
        }
        if (byte == TELNET_WILL || byte == TELNET_WONT ||
            byte == TELNET_DO   || byte == TELNET_DONT) {
            tn_pending_cmd = byte;
            tn_state = TELNET_STATE_OPTION;
            return false;
        }
        if (byte == TELNET_SB) {
            tn_state = TELNET_STATE_SUBNEG;
            return false;
        }
        /* NOP (241), AYT (246), GA (249), EL (248), EC (247),
         * IP (244), AO (245), BRK (243), DM (242), etc. — all ignored */
        tn_state = TELNET_STATE_DATA;
        return false;

    case TELNET_STATE_OPTION:
        /* This byte is the option code. Handle and go back to DATA. */
        tn_handle_command(tn_pending_cmd, byte);
        tn_state = TELNET_STATE_DATA;
        return false;

    case TELNET_STATE_SUBNEG:
        /* Discard everything inside subnegotiation */
        if (byte == TELNET_IAC) {
            tn_state = TELNET_STATE_SUBNEG_IAC;
        }
        return false;

    case TELNET_STATE_SUBNEG_IAC:
        if (byte == TELNET_SE) {
            tn_state = TELNET_STATE_DATA;  /* End of subnegotiation */
        } else {
            tn_state = TELNET_STATE_SUBNEG; /* Back inside SB       */
        }
        return false;
    }

    return false;  /* Unreachable */
}

bool telnet_is_binary_mode(void)
{
    return tn_binary_rx && tn_binary_tx;
}

bool telnet_negotiation_timed_out(void)
{
    if (telnet_is_binary_mode()) {
        return false;
    }
    TickType_t elapsed = xTaskGetTickCount() - tn_connect_ticks;
    return elapsed >= pdMS_TO_TICKS(CONFIG_SERIAL2NET_TELNET_NEGOTIATION_TIMEOUT_MS);
}

void telnet_reset(void)
{
    tn_sock           = -1;
    tn_state          = TELNET_STATE_DATA;
    tn_binary_rx      = false;
    tn_binary_tx      = false;
    tn_connect_ticks  = 0;
}
