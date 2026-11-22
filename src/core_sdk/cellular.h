#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Built-in cellular modem: voice calls, text messages and USSD.
 *
 * The modem stays on once enabled (also across reboots) so calls and
 * messages can come in. An incoming call shows a notification and opens the
 * phone app; one that ends unanswered shows a missed-call notification.
 * Ended calls are kept in a small log since boot, so the phone app can record
 * them even when it was not running. Received messages stay on the SIM, with
 * a notification, until an app reads and deletes them. Mobile data, when
 * turned on, gives apps internet access over the modem.
 */

#define BRUCE_CELLULAR_NUMBER_MAX 32
#define BRUCE_CELLULAR_OPERATOR_MAX 24
#define BRUCE_CELLULAR_LOG_MAX 16
#define BRUCE_CELLULAR_TEXT_MAX 400
/* One message: 160 characters of plain ASCII, otherwise 70 (UCS2). */
#define BRUCE_CELLULAR_SMS_ASCII_MAX 160
#define BRUCE_CELLULAR_SMS_UCS2_MAX 70
#define BRUCE_CELLULAR_APN_MAX 64

typedef enum {
    BRUCE_CELLULAR_OFF = 0,
    BRUCE_CELLULAR_STARTING,
    BRUCE_CELLULAR_FAILED, /**< The modem did not answer. */
    BRUCE_CELLULAR_NO_SIM,
    BRUCE_CELLULAR_SIM_LOCKED, /**< The SIM asks for a PIN. */
    BRUCE_CELLULAR_SEARCHING,
    BRUCE_CELLULAR_DENIED, /**< The network refused registration. */
    BRUCE_CELLULAR_READY,
} bruce_cellular_state_t;

typedef enum {
    BRUCE_CALL_NONE = 0,
    BRUCE_CALL_DIALING,
    BRUCE_CALL_ALERTING, /**< The other phone is ringing. */
    BRUCE_CALL_INCOMING,
    BRUCE_CALL_ACTIVE,
} bruce_call_state_t;

typedef enum {
    BRUCE_CALL_RESULT_ANSWERED = 0,
    BRUCE_CALL_RESULT_MISSED,    /**< Incoming, never answered. */
    BRUCE_CALL_RESULT_REJECTED,  /**< Incoming, declined here. */
    BRUCE_CALL_RESULT_CANCELLED, /**< Outgoing, hung up before it was answered. */
    BRUCE_CALL_RESULT_BUSY,
    BRUCE_CALL_RESULT_NO_ANSWER,
    BRUCE_CALL_RESULT_FAILED,
} bruce_call_result_t;

typedef struct {
    bruce_cellular_state_t state;
    int8_t signal_bars; /**< 0 to 4, or -1 when unknown. */
    char operator_name[BRUCE_CELLULAR_OPERATOR_MAX];
    bruce_call_state_t call;
    bool call_incoming;
    char call_number[BRUCE_CELLULAR_NUMBER_MAX];
    uint32_t call_seconds; /**< Talk time of the active call. */
    bool muted;
    uint8_t volume;             /**< 0 to 100. */
    uint32_t last_log_sequence; /**< Sequence of the newest call log record. */
    uint32_t message_sequence;  /**< Goes up with every message received since boot. */
} bruce_cellular_status_t;

typedef struct {
    uint32_t sequence; /**< 1 for the first call since boot. */
    bool incoming;
    bruce_call_result_t result;
    char number[BRUCE_CELLULAR_NUMBER_MAX];
    int64_t started_utc; /**< Unix seconds, or 0 when the clock was not set. */
    uint32_t seconds;    /**< Talk time. */
} bruce_cellular_call_record_t;

typedef struct {
    uint16_t id;                            /**< Where the SIM keeps it, for cellular__delete_message(). */
    int64_t received_utc;                   /**< Unix seconds from the network's timestamp, or 0. */
    char number[BRUCE_CELLULAR_NUMBER_MAX]; /**< The sender; may be a name such as "Orange". */
    char text[BRUCE_CELLULAR_TEXT_MAX];     /**< UTF-8; each part of a long message is its own message. */
} bruce_cellular_message_t;

typedef enum {
    BRUCE_CELLULAR_DATA_OFF = 0,
    BRUCE_CELLULAR_DATA_WAITING,    /**< On, waiting for the modem to register. */
    BRUCE_CELLULAR_DATA_CONNECTING,
    BRUCE_CELLULAR_DATA_CONNECTED,
    BRUCE_CELLULAR_DATA_FAILED, /**< The modem could not start a data link; tried again when turned on again. */
} bruce_cellular_data_state_t;

typedef struct {
    bool enabled;
    bruce_cellular_data_state_t state;
    char apn[BRUCE_CELLULAR_APN_MAX];
    char ip[16]; /**< Dotted IPv4 address while connected, else empty. */
} bruce_cellular_data_status_t;

/** @brief Returns whether the board has a cellular modem. Not permission-gated. */
bool cellular__is_available(void);

/**
 * @brief Turns the modem on or off; remembered across reboots.
 *
 * Turning it on returns at once; the state goes from STARTING to READY (or
 * an error state) within about 20 seconds.
 *
 * @param enabled Whether the modem should be on.
 * @permission phone
 */
bruce_result_t cellular__set_enabled(bool enabled);

/**
 * @brief Reads the modem, network and call state.
 *
 * @param out Receives the state.
 * @permission phone
 */
bruce_result_t cellular__get_status(bruce_cellular_status_t *out);

/**
 * @brief Calls a number (digits, '+', '*' and '#').
 *
 * @param number The number to call.
 * @permission phone
 */
bruce_result_t cellular__dial(const char *number);

/**
 * @brief Answers the incoming call.
 *
 * @permission phone
 */
bruce_result_t cellular__answer(void);

/**
 * @brief Ends the current call, or declines an incoming one.
 *
 * @permission phone
 */
bruce_result_t cellular__hang_up(void);

/**
 * @brief Sends a keypad tone during a call.
 *
 * @param digit One of 0-9, '*', '#'.
 * @permission phone
 */
bruce_result_t cellular__send_dtmf(char digit);

/**
 * @brief Mutes or unmutes the microphone during a call.
 *
 * @param muted Whether the microphone is muted.
 * @permission phone
 */
bruce_result_t cellular__set_muted(bool muted);

/**
 * @brief Sets the call volume.
 *
 * @param volume 0 to 100.
 * @permission phone
 */
bruce_result_t cellular__set_volume(uint8_t volume);

/**
 * @brief Sends a USSD code such as "*100#" and waits for the network's reply.
 *
 * @param code The code to send.
 * @param out_reply Receives the reply as UTF-8.
 * @param capacity Size of out_reply in bytes.
 * @param timeout_ms How long to wait for the reply.
 * @permission phone
 */
bruce_result_t cellular__ussd(const char *code, char *out_reply, size_t capacity, uint32_t timeout_ms);

/**
 * @brief Copies call log records newer than after_sequence, oldest first.
 *
 * Returns the number of records copied.
 *
 * @param after_sequence Only records with a larger sequence are copied; 0 for all.
 * @param out Receives the records.
 * @param capacity Number of records out can hold.
 * @permission phone
 */
size_t cellular__get_call_log(uint32_t after_sequence, bruce_cellular_call_record_t *out, size_t capacity);

/**
 * @brief Copies the received messages stored on the SIM, oldest slot first.
 *
 * Messages stay on the SIM until deleted, so an app saves them and then
 * calls cellular__delete_message() for each; a SIM holds only a few dozen.
 * Reading takes up to a few seconds.
 *
 * @param out Receives the messages.
 * @param capacity Number of messages out can hold.
 * @param out_count Receives the number copied.
 * @permission phone
 */
bruce_result_t cellular__get_messages(bruce_cellular_message_t *out, size_t capacity, size_t *out_count);

/**
 * @brief Deletes a received message from the SIM.
 *
 * @param id The message's id from cellular__get_messages().
 * @permission phone
 */
bruce_result_t cellular__delete_message(uint16_t id);

/**
 * @brief Sends a text message and waits until the network takes it.
 *
 * Plain ASCII fits BRUCE_CELLULAR_SMS_ASCII_MAX characters; any other text,
 * or a line break, is sent as UCS2 and fits BRUCE_CELLULAR_SMS_UCS2_MAX.
 * Longer text returns BRUCE_ERR_RESOURCE_LIMIT.
 *
 * @param number The recipient's number.
 * @param text The message, UTF-8.
 * @permission phone
 */
bruce_result_t cellular__send_message(const char *number, const char *text);

/**
 * @brief Turns mobile data on or off; remembered across reboots.
 *
 * While on and the modem is registered, Core keeps a data link up and
 * reconnects it when it drops. Apps then reach the internet over it with the
 * usual network calls; Wi-Fi is preferred while it is connected. Calls and
 * messages keep working during a data link.
 *
 * @param enabled Whether mobile data should be on.
 * @permission phone
 */
bruce_result_t cellular__set_data(bool enabled);

/**
 * @brief Sets the access point name for mobile data; remembered across reboots.
 *
 * An empty name uses whatever the modem and network pick, which works for
 * most SIMs. A running link reconnects with the new name.
 *
 * @param apn Up to BRUCE_CELLULAR_APN_MAX - 1 printable characters, no quotes.
 * @permission phone
 */
bruce_result_t cellular__set_apn(const char *apn);

/**
 * @brief Reads the mobile data state.
 *
 * @param out Receives the state.
 * @permission phone
 */
bruce_result_t cellular__get_data_status(bruce_cellular_data_status_t *out);
