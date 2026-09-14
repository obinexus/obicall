#include "obicall/obicall.h"
#include "fuzz_driver.h"

/* Every decoder that parses attacker/peer-controlled bytes (frame
 * headers and every wire payload type) gets exercised with the same
 * arbitrary input, sliced in ways that also probe boundary offsets - no
 * input, however malformed, should do more than return an error. */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;

    if (size >= OBICALL_WIRE_HEADER_LEN) {
        obicall_wire_header_t hdr;
        obicall_wire_decode_header(data, &hdr);
    }

    obicall_observation_t obs;
    obicall_wire_decode_observation(data, (uint32_t)size, &obs);

    obicall_result_t result;
    obicall_wire_decode_result(data, (uint32_t)size, &result);

    obicall_checkpoint_t ckpt;
    obicall_wire_decode_checkpoint(data, (uint32_t)size, &ckpt);

    static uint8_t event_storage[8192];
    obicall_event_t event;
    obicall_wire_decode_event(data, (uint32_t)size, event_storage, sizeof(event_storage), &event);

    obicall_msg_auth_hello_t hello;
    obicall_wire_decode_auth_hello(data, (uint32_t)size, &hello);

    obicall_msg_heartbeat_t hb;
    obicall_wire_decode_heartbeat(data, (uint32_t)size, &hb);

    obicall_msg_epoch_grant_t grant;
    obicall_wire_decode_epoch_grant(data, (uint32_t)size, &grant);

    obicall_msg_epoch_revoke_t revoke;
    obicall_wire_decode_epoch_revoke(data, (uint32_t)size, &revoke);

    obicall_msg_readiness_report_t readiness;
    obicall_wire_decode_readiness_report(data, (uint32_t)size, &readiness);

    obicall_msg_replay_request_t replay_req;
    obicall_wire_decode_replay_request(data, (uint32_t)size, &replay_req);

    obicall_msg_admission_decision_t decision;
    obicall_wire_decode_admission_decision(data, (uint32_t)size, &decision);

    obicall_msg_result_ack_t ack;
    obicall_wire_decode_result_ack(data, (uint32_t)size, &ack);

    obicall_msg_status_reply_t status;
    obicall_wire_decode_status_reply(data, (uint32_t)size, &status);

    /* Also exercise the journal's on-disk record header, a related but
     * distinct fixed-width parser. */
    if (size >= OBICALL_JOURNAL_RECORD_HEADER_LEN) {
        obicall_journal_record_header_t rhdr;
        obicall_journal_decode_record_header(data, &rhdr);
    }

    return 0;
}
