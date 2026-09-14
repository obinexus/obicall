#ifndef OBICALL_DGT_H
#define OBICALL_DGT_H

#include <stdint.h>

#include "obicall/platform.h"
#include "obicall/types.h"
#include "obicall/status.h"
#include "obicall/manifest.h"
#include "obicall/obicall_export.h"

OBICALL_BEGIN_DECLS

/*
 * Dimensional Game Theory policy, restricted to what docs/DGT.md documents
 * as a defensible instance: a versioned action/scenario loss table and a
 * constrained (eligibility-filtered) minimax action selector. This is not
 * an implementation of the DGT manuscript's general claims (see docs/DGT.md
 * for why the manuscript's "perfect play implies tie" premise does not
 * hold and is not relied on here) - actions are approved
 * (sensor-subset, estimator-mode, provider) combinations, and the
 * "opponent" is a fixed catalog of fault scenarios, not a strategic agent.
 */

#define OBICALL_DGT_POLICY_SCHEMA_VERSION 1u
#define OBICALL_DGT_MAX_ACTIONS 16u
#define OBICALL_DGT_MAX_SCENARIOS 16u
#define OBICALL_DGT_INVALID_ACTION_INDEX 0xFFFFFFFFu

typedef struct obicall_dgt_action {
    uint32_t action_id;
    char name[OBICALL_MAX_NAME_LEN];
    uint64_t sensor_subset_mask;
    uint32_t estimator_mode;
    char provider_name[OBICALL_MAX_NAME_LEN];
} obicall_dgt_action_t;

typedef struct obicall_dgt_scenario {
    uint32_t scenario_id;
    char name[OBICALL_MAX_NAME_LEN];
} obicall_dgt_scenario_t;

/* Dimensionless components before weighting; each must be finite and >= 0. */
typedef struct obicall_dgt_cost_components {
    double estimation_error;
    double delay;
    double coverage_loss;
    double resource_cost;
} obicall_dgt_cost_components_t;

typedef struct obicall_dgt_weights {
    double w_estimation_error;
    double w_delay;
    double w_coverage_loss;
    double w_resource_cost;
} obicall_dgt_weights_t;

typedef struct obicall_dgt_policy_table {
    uint32_t struct_size;
    uint32_t schema_version;
    uint32_t policy_version;

    uint32_t action_count;
    obicall_dgt_action_t actions[OBICALL_DGT_MAX_ACTIONS];

    uint32_t scenario_count;
    obicall_dgt_scenario_t scenarios[OBICALL_DGT_MAX_SCENARIOS];

    obicall_dgt_weights_t weights;

    /* cost[a][s]: cost components for actions[a] under scenarios[s]. */
    obicall_dgt_cost_components_t cost[OBICALL_DGT_MAX_ACTIONS][OBICALL_DGT_MAX_SCENARIOS];

    double scale_estimation_error; /* normalization scales, all > 0 */
    double scale_delay;
    double scale_coverage_loss;
    double scale_resource_cost;
} obicall_dgt_policy_table_t;

typedef struct obicall_dgt_eligibility {
    /* Indexed by action index (not action_id). Structural eligibility -
     * freshness/observability/capability/ownership - not scenario cost. */
    uint32_t action_eligible[OBICALL_DGT_MAX_ACTIONS];
} obicall_dgt_eligibility_t;

typedef struct obicall_dgt_hysteresis_state {
    uint32_t has_previous;
    uint32_t previous_action_index;
    double previous_worst_case_loss;
} obicall_dgt_hysteresis_state_t;

/* Action must strictly beat the held action's worst-case loss by more than
 * this fraction of the held loss to trigger a switch. */
#define OBICALL_DGT_DEFAULT_HYSTERESIS_MARGIN 0.05

typedef struct obicall_dgt_decision {
    uint32_t selected_action_id;
    uint32_t selected_action_index;
    double worst_case_loss;
    uint32_t eligible_count;
    uint32_t used_fallback_baseline;
    uint32_t used_hysteresis_hold;
} obicall_dgt_decision_t;

/* Checks finiteness/non-negativity of every cost component and weight,
 * positive scales, at least one positive weight, and struct_size/
 * schema_version. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_dgt_validate_table(
    const obicall_dgt_policy_table_t* table, char* out_issue_detail, uint32_t detail_cap);

OBICALL_API obicall_status_t OBICALL_CALL obicall_dgt_action_worst_case_loss(
    const obicall_dgt_policy_table_t* table, uint32_t action_index, double* out_loss);

/* Eliminates ineligible actions, then among the rest picks the action
 * minimizing worst_case_loss (min over actions of max over scenarios),
 * breaking ties by lowest action_id. Applies hysteresis_in/out only among
 * currently-eligible actions - a previously-held action that has since
 * become ineligible is dropped unconditionally, never retained by
 * hysteresis. Falls back to baseline_action_index (and sets
 * used_fallback_baseline) if the table is invalid; returns
 * OBICALL_ERR_DGT_NO_ELIGIBLE_ACTION if even the baseline is ineligible or
 * absent, meaning the caller must report invalid output rather than act on
 * out_decision. */
OBICALL_API obicall_status_t OBICALL_CALL obicall_dgt_select_action(
    const obicall_dgt_policy_table_t* table, const obicall_dgt_eligibility_t* eligibility,
    const obicall_dgt_hysteresis_state_t* hysteresis_in, double hysteresis_margin,
    uint32_t baseline_action_index, obicall_dgt_decision_t* out_decision,
    obicall_dgt_hysteresis_state_t* hysteresis_out);

OBICALL_END_DECLS

#endif /* OBICALL_DGT_H */
