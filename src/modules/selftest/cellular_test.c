#include "cellular_test.h"

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
                   model.signal_bars == 3 && strcmp(model.operator_name, "Orange PL") == 0;
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
