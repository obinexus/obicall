#include "obicall_test.h"
#include "obicall/obicall.h"

static obicall_result_t make_result(const char* pipeline_id, uint64_t window_seq, uint32_t broker_id,
                                     uint64_t epoch, int64_t now, int64_t validity_ns) {
    obicall_result_t r;
    memset(&r, 0, sizeof(r));
    r.struct_size = sizeof(r);
    r.schema_version = OBICALL_RESULT_SCHEMA_VERSION;
    strncpy(r.pipeline_id, pipeline_id, OBICALL_PIPELINE_ID_LEN - 1);
    r.window_seq = window_seq;
    r.broker_id = broker_id;
    r.epoch = epoch;
    r.status = OBICALL_RESULT_VALID;
    r.payload_shape = OBICALL_SHAPE_POSITION_2D;
    r.payload_count = 2;
    r.payload[0] = 1.0;
    r.payload[1] = 2.0;
    r.covariance_count = 4;
    r.covariance[0] = 0.1;
    r.covariance[3] = 0.1;
    r.timestamp_ns = now;
    r.valid_until_ns = now + validity_ns;
    return r;
}

static void init_state(obicall_gate_state_t* s) {
    memset(s, 0, sizeof(*s));
    s->struct_size = sizeof(*s);
    s->schema_version = 1;
}

static void test_no_owner_rejects_everything(void) {
    obicall_gate_state_t s;
    init_state(&s);
    obicall_result_t r = make_result("p", 1, OBICALL_BROKER_A, 1, 1000, 500000000LL);
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &r, 1000, NULL) == OBICALL_GATE_REJECT_NOT_OWNER);
}

static void test_owner_commits_first_result(void) {
    obicall_gate_state_t s;
    init_state(&s);
    uint64_t epoch;
    OBICALL_CHECK(obicall_gate_promote(&s, OBICALL_BROKER_A, 0, &epoch) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(epoch, 1);

    obicall_result_t r = make_result("p", 1, OBICALL_BROKER_A, 1, 1000, 500000000LL);
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &r, 1000, NULL) == OBICALL_GATE_COMMIT);
    OBICALL_CHECK_EQ_INT(s.pipeline_count, 1);
    OBICALL_CHECK_EQ_INT(s.pipelines[0].last_committed_window_seq, 1);
}

static void test_duplicate_window_rejected(void) {
    obicall_gate_state_t s;
    init_state(&s);
    uint64_t epoch;
    obicall_gate_promote(&s, OBICALL_BROKER_A, 0, &epoch);

    obicall_result_t r1 = make_result("p", 5, OBICALL_BROKER_A, 1, 1000, 500000000LL);
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &r1, 1000, NULL) == OBICALL_GATE_COMMIT);

    /* Same window again (even with different content) must not commit a
     * second time. */
    obicall_result_t r2 = make_result("p", 5, OBICALL_BROKER_A, 1, 2000, 500000000LL);
    r2.payload[0] = 999.0;
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &r2, 2000, NULL) == OBICALL_GATE_REJECT_DUPLICATE_WINDOW);

    /* An older window than the high-water mark is also rejected. */
    obicall_result_t r3 = make_result("p", 3, OBICALL_BROKER_A, 1, 2000, 500000000LL);
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &r3, 2000, NULL) == OBICALL_GATE_REJECT_DUPLICATE_WINDOW);
}

static void test_expired_result_rejected(void) {
    obicall_gate_state_t s;
    init_state(&s);
    uint64_t epoch;
    obicall_gate_promote(&s, OBICALL_BROKER_A, 0, &epoch);
    obicall_result_t r = make_result("p", 1, OBICALL_BROKER_A, 1, 1000, 100LL); /* valid_until = 1100 */
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &r, 5000 /* now, past valid_until */, NULL) ==
                  OBICALL_GATE_REJECT_EXPIRED);
}

static void test_non_owner_recorded_not_committed(void) {
    obicall_gate_state_t s;
    init_state(&s);
    uint64_t epoch;
    obicall_gate_promote(&s, OBICALL_BROKER_A, 0, &epoch); /* A is owner, epoch 1 */

    /* B submits at the CURRENT epoch (a legitimate shadow candidate, not
     * a stale leftover) - must be recorded for comparison, not
     * committed, since only the gate's grant can make B the owner. */
    obicall_result_t rb = make_result("p", 1, OBICALL_BROKER_B, 1, 1000, 500000000LL);
    uint32_t pidx = 0xFFFFFFFFu;
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &rb, 1000, &pidx) == OBICALL_GATE_SHADOW_RECORDED);
    /* A pipeline tracking slot may be allocated as bookkeeping (so a
     * shadow's future submissions have somewhere to land), but nothing
     * must be marked committed by a non-owner's submission. */
    OBICALL_CHECK(pidx < s.pipeline_count);
    OBICALL_CHECK_EQ_INT(s.pipelines[pidx].has_committed, 0);
}

static void test_stale_epoch_after_promotion_rejected(void) {
    obicall_gate_state_t s;
    init_state(&s);
    uint64_t epoch1, epoch2;
    obicall_gate_promote(&s, OBICALL_BROKER_A, 0, &epoch1); /* epoch 1, A owns */
    obicall_result_t ra = make_result("p", 1, OBICALL_BROKER_A, epoch1, 1000, 500000000LL);
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &ra, 1000, NULL) == OBICALL_GATE_COMMIT);

    obicall_gate_promote(&s, OBICALL_BROKER_B, 2000, &epoch2); /* B takes over, epoch 2 */
    OBICALL_CHECK_EQ_INT(epoch2, 2);

    /* A delayed result from the old owner, still claiming the old
     * epoch, must be rejected after promotion - this is the fencing
     * invariant (Chubby-style sequencer, per docs/ARCHITECTURE.md). */
    obicall_result_t stale = make_result("p", 2, OBICALL_BROKER_A, epoch1, 3000, 500000000LL);
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &stale, 3000, NULL) == OBICALL_GATE_REJECT_STALE_EPOCH);

    /* B publishing at the new epoch must succeed. */
    obicall_result_t rb = make_result("p", 2, OBICALL_BROKER_B, epoch2, 3000, 500000000LL);
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &rb, 3000, NULL) == OBICALL_GATE_COMMIT);
}

static void test_promote_revokes_before_granting(void) {
    obicall_gate_state_t s;
    init_state(&s);
    uint64_t epoch1, epoch2;
    obicall_gate_promote(&s, OBICALL_BROKER_A, 0, &epoch1);
    OBICALL_CHECK_EQ_INT(s.owner_broker_id, OBICALL_BROKER_A);
    obicall_gate_promote(&s, OBICALL_BROKER_B, 1000, &epoch2);
    OBICALL_CHECK_EQ_INT(s.owner_broker_id, OBICALL_BROKER_B);
    OBICALL_CHECK(epoch2 > epoch1); /* epoch strictly increases across the revoke+grant */
}

static void test_shadow_promotable_requires_matching_readiness(void) {
    obicall_gate_readiness_t ready;
    memset(&ready, 0, sizeof(ready));
    ready.healthy = 1;
    memset(ready.config_digest, 0xAB, OBICALL_DIGEST_LEN);
    ready.checkpoint_schema_version = 1;
    ready.last_processed_window_seq = 100;

    obicall_gate_promotion_requirements_t req;
    memcpy(req.required_config_digest, ready.config_digest, OBICALL_DIGEST_LEN);
    req.required_checkpoint_schema_version = 1;
    req.max_allowed_replay_gap = 10;

    OBICALL_CHECK(obicall_gate_shadow_promotable(&ready, &req, 105) != 0); /* within gap */
    OBICALL_CHECK(obicall_gate_shadow_promotable(&ready, &req, 200) == 0); /* too far behind */

    ready.healthy = 0;
    OBICALL_CHECK(obicall_gate_shadow_promotable(&ready, &req, 100) == 0); /* unhealthy */
    ready.healthy = 1;

    ready.checkpoint_schema_version = 2; /* schema mismatch */
    OBICALL_CHECK(obicall_gate_shadow_promotable(&ready, &req, 100) == 0);
    ready.checkpoint_schema_version = 1;

    memset(ready.config_digest, 0xCD, OBICALL_DIGEST_LEN); /* config mismatch */
    OBICALL_CHECK(obicall_gate_shadow_promotable(&ready, &req, 100) == 0);
}

static void test_results_agree_tolerance(void) {
    obicall_result_t a = make_result("p", 1, OBICALL_BROKER_A, 1, 0, 1000);
    obicall_result_t b = make_result("p", 1, OBICALL_BROKER_B, 1, 0, 1000);
    OBICALL_CHECK(obicall_gate_results_agree(&a, &b, 1e-6, 1e-6) != 0); /* identical: agree */

    b.payload[0] = a.payload[0] + 0.5;
    OBICALL_CHECK(obicall_gate_results_agree(&a, &b, 1.0, 1e-6) != 0); /* within tolerance */
    OBICALL_CHECK(obicall_gate_results_agree(&a, &b, 0.1, 1e-6) == 0); /* outside tolerance: disagreement */
}

static void test_persist_and_load_roundtrip(void) {
    obicall_gate_state_t s;
    init_state(&s);
    uint64_t epoch;
    obicall_gate_promote(&s, OBICALL_BROKER_A, 0, &epoch);
    obicall_result_t r = make_result("position-fusion", 17, OBICALL_BROKER_A, epoch, 1000, 500000000LL);
    obicall_gate_decide_publish(&s, &r, 1000, NULL);

    FILE* f = tmpfile();
    OBICALL_CHECK(f != NULL);
    if (!f) return;
    OBICALL_CHECK(obicall_gate_persist_state(f, &s) == OBICALL_OK);
    rewind(f);

    obicall_gate_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    OBICALL_CHECK(obicall_gate_load_state(f, &loaded) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(loaded.epoch, s.epoch);
    OBICALL_CHECK_EQ_INT(loaded.owner_broker_id, s.owner_broker_id);
    OBICALL_CHECK_EQ_INT(loaded.pipeline_count, 1);
    OBICALL_CHECK(strcmp(loaded.pipelines[0].pipeline_id, "position-fusion") == 0);
    OBICALL_CHECK_EQ_INT(loaded.pipelines[0].last_committed_window_seq, 17);
    fclose(f);
}

static void test_load_detects_corruption(void) {
    obicall_gate_state_t s;
    init_state(&s);
    uint64_t epoch;
    obicall_gate_promote(&s, OBICALL_BROKER_A, 0, &epoch);

    FILE* f = tmpfile();
    OBICALL_CHECK(f != NULL);
    if (!f) return;
    OBICALL_CHECK(obicall_gate_persist_state(f, &s) == OBICALL_OK);

    /* Flip a byte inside the persisted payload - the checksum must catch
     * this rather than silently loading corrupted state. */
    fseek(f, 20, SEEK_SET);
    uint8_t original;
    fread(&original, 1, 1, f);
    fseek(f, 20, SEEK_SET);
    uint8_t corrupted = (uint8_t)(original ^ 0xFF);
    fwrite(&corrupted, 1, 1, f);
    rewind(f);

    obicall_gate_state_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    obicall_status_t st = obicall_gate_load_state(f, &loaded);
    OBICALL_CHECK(st == OBICALL_ERR_WIRE_CHECKSUM || st == OBICALL_ERR_WIRE_MALFORMED);
    fclose(f);
}

static void test_incompatible_result_rejected(void) {
    obicall_gate_state_t s;
    init_state(&s);
    uint64_t epoch;
    obicall_gate_promote(&s, OBICALL_BROKER_A, 0, &epoch);
    obicall_result_t r = make_result("p", 1, OBICALL_BROKER_A, epoch, 1000, 500000000LL);
    r.schema_version = 9999; /* unsupported */
    OBICALL_CHECK(obicall_gate_decide_publish(&s, &r, 1000, NULL) == OBICALL_GATE_REJECT_INCOMPATIBLE);
}

OBICALL_TEST_MAIN_BEGIN()
    test_no_owner_rejects_everything();
    test_owner_commits_first_result();
    test_duplicate_window_rejected();
    test_expired_result_rejected();
    test_non_owner_recorded_not_committed();
    test_stale_epoch_after_promotion_rejected();
    test_promote_revokes_before_granting();
    test_shadow_promotable_requires_matching_readiness();
    test_results_agree_tolerance();
    test_persist_and_load_roundtrip();
    test_load_detects_corruption();
    test_incompatible_result_rejected();
OBICALL_TEST_MAIN_END()
