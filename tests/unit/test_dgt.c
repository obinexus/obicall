#include "obicall_test.h"
#include "obicall/obicall.h"

/* The spec's canonical synthetic fixture: three single-purpose actions
 * (camera-heavy, inertial-heavy, balanced) whose worst-case losses are
 * exactly 0.85, 0.70, and 0.35 - engineered via a single scenario, weight
 * 1 on estimation_error only, everything else zero, all scales 1, so the
 * worst-case loss of each action is just its own estimation_error. These
 * numbers are illustrative fixture values, not measurements - see
 * docs/DGT.md. */
static void make_fixture_table(obicall_dgt_policy_table_t* t) {
    memset(t, 0, sizeof(*t));
    t->struct_size = sizeof(*t);
    t->schema_version = OBICALL_DGT_POLICY_SCHEMA_VERSION;
    t->policy_version = 1;
    t->action_count = 3;
    t->scenario_count = 1;

    t->actions[0].action_id = 0;
    strncpy(t->actions[0].name, "camera_heavy", OBICALL_MAX_NAME_LEN - 1);
    t->actions[1].action_id = 1;
    strncpy(t->actions[1].name, "inertial_heavy", OBICALL_MAX_NAME_LEN - 1);
    t->actions[2].action_id = 2;
    strncpy(t->actions[2].name, "balanced", OBICALL_MAX_NAME_LEN - 1);

    t->scenarios[0].scenario_id = 0;
    strncpy(t->scenarios[0].name, "nominal", OBICALL_MAX_NAME_LEN - 1);

    t->cost[0][0].estimation_error = 0.85;
    t->cost[1][0].estimation_error = 0.70;
    t->cost[2][0].estimation_error = 0.35;

    t->weights.w_estimation_error = 1.0;
    t->scale_estimation_error = 1.0;
    t->scale_delay = 1.0;
    t->scale_coverage_loss = 1.0;
    t->scale_resource_cost = 1.0;
}

static void all_eligible(obicall_dgt_eligibility_t* e, uint32_t n) {
    memset(e, 0, sizeof(*e));
    for (uint32_t i = 0; i < n; ++i) e->action_eligible[i] = 1;
}

static void test_fixture_losses_are_exact(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    char detail[128];
    OBICALL_CHECK(obicall_dgt_validate_table(&t, detail, sizeof(detail)) == OBICALL_OK);

    double loss;
    OBICALL_CHECK(obicall_dgt_action_worst_case_loss(&t, 0, &loss) == OBICALL_OK);
    OBICALL_CHECK_NEAR(loss, 0.85, 1e-12);
    OBICALL_CHECK(obicall_dgt_action_worst_case_loss(&t, 1, &loss) == OBICALL_OK);
    OBICALL_CHECK_NEAR(loss, 0.70, 1e-12);
    OBICALL_CHECK(obicall_dgt_action_worst_case_loss(&t, 2, &loss) == OBICALL_OK);
    OBICALL_CHECK_NEAR(loss, 0.35, 1e-12);
}

static void test_balanced_action_wins_when_eligible(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    obicall_dgt_eligibility_t elig;
    all_eligible(&elig, 3);

    obicall_dgt_decision_t decision;
    OBICALL_CHECK(obicall_dgt_select_action(&t, &elig, NULL, OBICALL_DGT_DEFAULT_HYSTERESIS_MARGIN, 0, &decision,
                                             NULL) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(decision.selected_action_index, 2);
    OBICALL_CHECK_EQ_INT(decision.selected_action_id, 2);
    OBICALL_CHECK_NEAR(decision.worst_case_loss, 0.35, 1e-12);
    OBICALL_CHECK_EQ_INT(decision.used_fallback_baseline, 0);
}

static void test_balanced_action_excluded_when_ineligible(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    obicall_dgt_eligibility_t elig;
    all_eligible(&elig, 3);
    elig.action_eligible[2] = 0; /* balanced action becomes ineligible */

    obicall_dgt_decision_t decision;
    OBICALL_CHECK(obicall_dgt_select_action(&t, &elig, NULL, OBICALL_DGT_DEFAULT_HYSTERESIS_MARGIN, 0, &decision,
                                             NULL) == OBICALL_OK);
    OBICALL_CHECK(decision.selected_action_index != 2);
    OBICALL_CHECK_EQ_INT(decision.selected_action_index, 1); /* next-best: inertial_heavy at 0.70 */
    OBICALL_CHECK_NEAR(decision.worst_case_loss, 0.70, 1e-12);
}

static void test_deterministic_tie_break_by_action_id(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    t.cost[0][0].estimation_error = 0.5;
    t.cost[1][0].estimation_error = 0.5; /* tie between action 0 and 1 */
    t.cost[2][0].estimation_error = 0.9;
    obicall_dgt_eligibility_t elig;
    all_eligible(&elig, 3);

    obicall_dgt_decision_t decision;
    OBICALL_CHECK(obicall_dgt_select_action(&t, &elig, NULL, OBICALL_DGT_DEFAULT_HYSTERESIS_MARGIN, 0, &decision,
                                             NULL) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(decision.selected_action_index, 0); /* lower action_id wins ties */
}

static void test_hysteresis_holds_previous_within_margin(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    /* action 1 and action 2 are close enough that switching from 1->2
     * should be suppressed by a generous hysteresis margin. */
    t.cost[0][0].estimation_error = 0.90;
    t.cost[1][0].estimation_error = 0.50;
    t.cost[2][0].estimation_error = 0.48; /* only ~4% better than action 1 */
    obicall_dgt_eligibility_t elig;
    all_eligible(&elig, 3);

    obicall_dgt_hysteresis_state_t prev;
    prev.has_previous = 1;
    prev.previous_action_index = 1;
    prev.previous_worst_case_loss = 0.50;

    obicall_dgt_decision_t decision;
    obicall_dgt_hysteresis_state_t out;
    OBICALL_CHECK(obicall_dgt_select_action(&t, &elig, &prev, 0.10 /* 10% margin */, 0, &decision, &out) ==
                  OBICALL_OK);
    OBICALL_CHECK_EQ_INT(decision.selected_action_index, 1); /* held, even though action 2 is nominally better */
    OBICALL_CHECK_EQ_INT(decision.used_hysteresis_hold, 1);
}

static void test_hysteresis_switches_when_improvement_exceeds_margin(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    obicall_dgt_eligibility_t elig;
    all_eligible(&elig, 3);

    obicall_dgt_hysteresis_state_t prev;
    prev.has_previous = 1;
    prev.previous_action_index = 0; /* was on the worst action */
    prev.previous_worst_case_loss = 0.85;

    obicall_dgt_decision_t decision;
    obicall_dgt_hysteresis_state_t out;
    OBICALL_CHECK(obicall_dgt_select_action(&t, &elig, &prev, 0.05, 0, &decision, &out) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(decision.selected_action_index, 2); /* 0.35 is far more than 5% better than 0.85 */
    OBICALL_CHECK_EQ_INT(decision.used_hysteresis_hold, 0);
}

static void test_hysteresis_never_retains_now_ineligible_action(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    obicall_dgt_eligibility_t elig;
    all_eligible(&elig, 3);
    elig.action_eligible[2] = 0; /* the previously-held action is now unsafe/unavailable */

    obicall_dgt_hysteresis_state_t prev;
    prev.has_previous = 1;
    prev.previous_action_index = 2;
    prev.previous_worst_case_loss = 0.35;

    obicall_dgt_decision_t decision;
    obicall_dgt_hysteresis_state_t out;
    OBICALL_CHECK(obicall_dgt_select_action(&t, &elig, &prev, 0.05, 0, &decision, &out) == OBICALL_OK);
    /* Must not retain action 2 via hysteresis just because it used to be held. */
    OBICALL_CHECK(decision.selected_action_index != 2);
    OBICALL_CHECK_EQ_INT(decision.used_hysteresis_hold, 0);
}

static void test_invalid_table_falls_back_to_baseline(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    t.weights.w_estimation_error = -1.0; /* invalid: negative weight */
    obicall_dgt_eligibility_t elig;
    all_eligible(&elig, 3);

    obicall_dgt_decision_t decision;
    OBICALL_CHECK(obicall_dgt_select_action(&t, &elig, NULL, OBICALL_DGT_DEFAULT_HYSTERESIS_MARGIN, 1, &decision,
                                             NULL) == OBICALL_OK);
    OBICALL_CHECK_EQ_INT(decision.used_fallback_baseline, 1);
    OBICALL_CHECK_EQ_INT(decision.selected_action_index, 1);
}

static void test_no_eligible_action_and_ineligible_baseline_fails(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    obicall_dgt_eligibility_t elig;
    memset(&elig, 0, sizeof(elig)); /* nothing eligible at all */

    obicall_dgt_decision_t decision;
    obicall_status_t st =
        obicall_dgt_select_action(&t, &elig, NULL, OBICALL_DGT_DEFAULT_HYSTERESIS_MARGIN, 0, &decision, NULL);
    OBICALL_CHECK(st == OBICALL_ERR_DGT_NO_ELIGIBLE_ACTION);
}

static void test_validate_table_rejects_negative_cost(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    t.cost[0][0].delay = -0.1;
    char detail[128];
    OBICALL_CHECK(obicall_dgt_validate_table(&t, detail, sizeof(detail)) == OBICALL_ERR_DGT_INVALID_SCORE);
}

static void test_validate_table_requires_positive_weight(void) {
    obicall_dgt_policy_table_t t;
    make_fixture_table(&t);
    t.weights.w_estimation_error = 0.0; /* all weights zero: not allowed */
    char detail[128];
    OBICALL_CHECK(obicall_dgt_validate_table(&t, detail, sizeof(detail)) == OBICALL_ERR_DGT_INVALID_SCORE);
}

OBICALL_TEST_MAIN_BEGIN()
    test_fixture_losses_are_exact();
    test_balanced_action_wins_when_eligible();
    test_balanced_action_excluded_when_ineligible();
    test_deterministic_tie_break_by_action_id();
    test_hysteresis_holds_previous_within_margin();
    test_hysteresis_switches_when_improvement_exceeds_margin();
    test_hysteresis_never_retains_now_ineligible_action();
    test_invalid_table_falls_back_to_baseline();
    test_no_eligible_action_and_ineligible_baseline_fails();
    test_validate_table_rejects_negative_cost();
    test_validate_table_requires_positive_weight();
OBICALL_TEST_MAIN_END()
