#include "cellular_test.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/cellular/cellular.h"

static cellular_line_t selftest__cellular_feed(cellular_model_t *model, const char *line, uint64_t now_ms) {
    return cellular__parse_line(model, line, now_ms);
}

bool selftest__run_cellular_parse_case(void) {
    cellular_model_t model;
    cellular__model_reset(&model);

    /* Network state. */
    bool ok_final = selftest__cellular_feed(&model, "OK", 0).ok;
    selftest__cellular_feed(&model, "+CPIN: READY", 0);
    selftest__cellular_feed(&model, "+CREG: 1,2", 0);
    bool searching = cellular__model_state(&model) == BRUCE_CELLULAR_SEARCHING;
    selftest__cellular_feed(&model, "+CEREG: 1,\"1A2B\",\"01A2B3C4\",7", 0);
    selftest__cellular_feed(&model, "+CSQ: 18,99", 0);
    selftest__cellular_feed(&model, "+COPS: 0,0,\"Orange PL\",7", 0);
    bool network = searching && cellular__model_state(&model) == BRUCE_CELLULAR_READY &&
                   model.signal_bars == 3 && strcmp(model.operator_name, "Orange PL") == 0 &&
                   model.generation == 4;
    selftest__cellular_feed(&model, "+COPS: 0,0,\"Orange PL\",3", 0);
    network = network && model.generation == 2;
    selftest__cellular_feed(&model, "+COPS: 0,0,\"Orange PL\",6", 0);
    network = network && model.generation == 3;
    selftest__cellular_feed(&model, "+COPS: 0", 0);
    network = network && model.generation == 0 && model.operator_name[0] == '\0';
    selftest__cellular_feed(&model, "+COPS: 0,0,\"Orange PL\",7", 0);
    cellular_model_t no_sim;
    cellular__model_reset(&no_sim);
    bool no_sim_final = selftest__cellular_feed(&no_sim, "+CME ERROR: 10", 0).final;
    bool sim = no_sim_final && cellular__model_state(&no_sim) == BRUCE_CELLULAR_NO_SIM;

    /* Missed incoming call. */
    selftest__cellular_feed(&model, "RING", 1000);
    selftest__cellular_feed(&model, "+CLIP: \"+48123456789\",145,,,,0", 1000);
    bool ringing =
        model.call == BRUCE_CALL_INCOMING && model.ring && strcmp(model.number, "+48123456789") == 0;
    cellular_line_t missed = selftest__cellular_feed(&model, "NO CARRIER", 9000);
    bool missed_ok = missed.ended && missed.record.incoming &&
                     missed.record.result == BRUCE_CALL_RESULT_MISSED &&
                     strcmp(missed.record.number, "+48123456789") == 0 && model.call == BRUCE_CALL_NONE;

    /* Outgoing call, answered, ended by the other side (no more +CLCC lines). */
    cellular__model_dial(&model, "112", 20000, 0);
    selftest__cellular_feed(&model, "+CLCC: 1,0,3,0,0,\"112\",129", 21000);
    bool alerting = model.call == BRUCE_CALL_ALERTING;
    selftest__cellular_feed(&model, "+CLCC: 1,0,0,0,0,\"112\",129", 22000);
    bool active = model.call == BRUCE_CALL_ACTIVE && model.connected;
    bruce_cellular_call_record_t record = {0};
    bool still = !cellular__clcc_done(&model, 23000, &record);
    bool ended = cellular__clcc_done(&model, 52000, &record) && record.result == BRUCE_CALL_RESULT_ANSWERED &&
                 record.seconds == 30 && !record.incoming;

    /* An incoming call first seen in +CLCC (its RING lost) still rings. */
    cellular_model_t woken;
    cellular__model_reset(&woken);
    selftest__cellular_feed(&woken, "+CLCC: 1,1,4,0,0,\"+48600100200\",145", 30000);
    bool clcc_ring = woken.call == BRUCE_CALL_INCOMING && woken.ring && strcmp(woken.number, "+48600100200") == 0;
    ringing = ringing && clcc_ring;

    /* USSD in UCS2. */
    selftest__cellular_feed(&model, "+CUSD: 0,\"0053016B0061\",72", 0);
    bool ussd = model.ussd_done && strcmp(
                                       model.ussd,
                                       "S\xc5\xab"
                                       "a"
                                   ) == 0;
    cellular__model_reset(&model);
    selftest__cellular_feed(&model, "+CUSD: 0,\"Balance: 5.00 PLN\",15", 0);
    ussd = ussd && strcmp(model.ussd, "Balance: 5.00 PLN") == 0;

    bool ok =
        ok_final && network && sim && ringing && missed_ok && alerting && active && still && ended && ussd;
    printf(
        "[selftest] cellular/parse: %s (network=%d sim=%d ring=%d missed=%d alerting=%d active=%d end=%d/%d "
        "ussd=%d)\n",
        ok ? "OK" : "FAIL",
        network,
        sim,
        ringing,
        missed_ok,
        alerting,
        active,
        still,
        ended,
        ussd
    );
    return ok;
}

bool selftest__run_cellular_sms_case(void) {
    cellular_model_t model;
    cellular__model_reset(&model);
    cellular__parse_line(&model, "+CMTI: \"SM\",3", 0);
    cellular__parse_line(&model, "+CPMS: \"SM\",2,50,\"SM\",2,50,\"SM\",2,50", 0);
    bool urc = model.sms_index == 3 && model.sms_used == 2 && model.sms_total == 50;

    /* The time stamp holds a comma inside its quotes; +04 is UTC+1. */
    cellular_sms_header_t header;
    bool parsed = cellular__sms_header(
        "\"REC UNREAD\",\"+48600100200\",,\"24/11/21,10:00:00+04\",145,4,0,8,\"+48790998250\",145,8", &header
    );
    bool header_ok = parsed && header.received && strcmp(header.number, "+48600100200") == 0 &&
                     header.utc == 1732179600 && header.fo == 4 && header.dcs == 8;
    cellular__sms_header("\"STO SENT\",\"123\",,", &header);
    bool sent = !header.received;

    char text[64];
    cellular__sms_text("Hi there", 17, 0, text, sizeof(text));
    bool plain = strcmp(text, "Hi there") == 0;
    /* "Zażółć 😀" in UCS2, the emoji as a surrogate pair. */
    cellular__sms_text("005A0061017C00F3014201070020D83DDE00", 4, 8, text, sizeof(text));
    bool ucs2 = strcmp(text, "Za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 \xf0\x9f\x98\x80") == 0;
    /* Part of a long message: 6-octet header, then "Hi" in GSM 7-bit after a fill bit. */
    cellular__sms_text("050003AB02019069", 0x44, 0, text, sizeof(text));
    bool long_part = strcmp(text, "Hi") == 0;

    char hex[32];
    bool encode = cellular__sms_ascii("Hello, world!") && !cellular__sms_ascii("a\nb") &&
                  cellular__sms_ucs2("\xc5\xbc!", 70, hex, sizeof(hex)) == 2 &&
                  strcmp(hex, "017C0021") == 0 && cellular__sms_ucs2("abc", 2, hex, sizeof(hex)) == 0 &&
                  cellular__sms_ucs2("\xf0\x9f\x98\x80", 70, NULL, SIZE_MAX) == 2;

    bool ok = urc && header_ok && sent && plain && ucs2 && long_part && encode;
    printf(
        "[selftest] cellular/sms: %s (urc=%d header=%d sent=%d plain=%d ucs2=%d long=%d encode=%d)\n",
        ok ? "OK" : "FAIL",
        urc,
        header_ok,
        sent,
        plain,
        ucs2,
        long_part,
        encode
    );
    return ok;
}
