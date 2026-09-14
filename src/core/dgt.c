#include "obicall/dgt.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

static void set_issue(char* out, uint32_t cap, const char* msg) {
    if (out && cap > 0) snprintf(out, cap, "%s", msg);
}

obicall_status_t OBICALL_CALL obicall_dgt_validate_table(const obicall_dgt_policy_table_t* table,
                                                           char* out_issue_detail,
                                                           uint32_t detail_cap) {
    if (!table) return OBICALL_ERR_NULL_POINTER;
    if (table->struct_size != sizeof(*table)) {
        set_issue(out_issue_detail, detail_cap, "struct_size mismatch");
        return OBICALL_ERR_INVALID_ARGUMENT;
    }
    if (table->schema_version != OBICALL_DGT_POLICY_SCHEMA_VERSION) {
        set_issue(out_issue_detail, detail_cap, "unsupported policy schema_version");
        return OBICALL_ERR_UNSUPPORTED;
    }
    if (table->action_count == 0 || table->action_count > OBICALL_DGT_MAX_ACTIONS) {
        set_issue(out_issue_detail, detail_cap, "action_count out of range");
        return OBICALL_ERR_INVALID_ARGUMENT;
    }
    if (table->scenario_count == 0 || table->scenario_count > OBICALL_DGT_MAX_SCENARIOS) {
        set_issue(out_issue_detail, detail_cap, "scenario_count out of range");
        return OBICALL_ERR_INVALID_ARGUMENT;
    }

    const obicall_dgt_weights_t* w = &table->weights;
    double weights[4] = {w->w_estimation_error, w->w_delay, w->w_coverage_loss, w->w_resource_cost};
    int any_positive = 0;
    for (int i = 0; i < 4; ++i) {
        if (!isfinite(weights[i]) || weights[i] < 0.0) {
            set_issue(out_issue_detail, detail_cap, "weight is non-finite or negative");
            return OBICALL_ERR_DGT_INVALID_SCORE;
        }
        if (weights[i] > 0.0) any_positive = 1;
    }
    if (!any_positive) {
        set_issue(out_issue_detail, detail_cap, "at least one weight must be positive");
        return OBICALL_ERR_DGT_INVALID_SCORE;
    }

    double scales[4] = {table->scale_estimation_error, table->scale_delay, table->scale_coverage_loss,
                         table->scale_resource_cost};
    for (int i = 0; i < 4; ++i) {
        if (!isfinite(scales[i]) || scales[i] <= 0.0) {
            set_issue(out_issue_detail, detail_cap, "normalization scale must be finite and positive");
            return OBICALL_ERR_DGT_INVALID_SCORE;
        }
    }

    for (uint32_t a = 0; a < table->action_count; ++a) {
        for (uint32_t s = 0; s < table->scenario_count; ++s) {
            const obicall_dgt_cost_components_t* c = &table->cost[a][s];
            double comps[4] = {c->estimation_error, c->delay, c->coverage_loss, c->resource_cost};
            for (int i = 0; i < 4; ++i) {
                if (!isfinite(comps[i]) || comps[i] < 0.0) {
                    set_issue(out_issue_detail, detail_cap, "cost component is non-finite or negative");
                    return OBICALL_ERR_DGT_INVALID_SCORE;
                }
            }
        }
    }
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_dgt_action_worst_case_loss(
    const obicall_dgt_policy_table_t* table, uint32_t action_index, double* out_loss) {
    if (!table || !out_loss) return OBICALL_ERR_NULL_POINTER;
    if (action_index >= table->action_count) return OBICALL_ERR_OUT_OF_RANGE;

    double worst = 0.0;
    int have_one = 0;
    for (uint32_t s = 0; s < table->scenario_count; ++s) {
        const obicall_dgt_cost_components_t* c = &table->cost[action_index][s];
        double loss = table->weights.w_estimation_error * (c->estimation_error / table->scale_estimation_error) +
                      table->weights.w_delay * (c->delay / table->scale_delay) +
                      table->weights.w_coverage_loss * (c->coverage_loss / table->scale_coverage_loss) +
                      table->weights.w_resource_cost * (c->resource_cost / table->scale_resource_cost);
        if (!isfinite(loss)) return OBICALL_ERR_DGT_INVALID_SCORE;
        if (!have_one || loss > worst) { worst = loss; have_one = 1; }
    }
    if (!have_one) return OBICALL_ERR_DGT_INVALID_SCORE;
    *out_loss = worst;
    return OBICALL_OK;
}

static obicall_status_t select_baseline(const obicall_dgt_policy_table_t* table,
                                         const obicall_dgt_eligibility_t* eligibility,
                                         uint32_t baseline_action_index,
                                         obicall_dgt_decision_t* out_decision,
                                         obicall_dgt_hysteresis_state_t* hysteresis_out) {
    if (!table || baseline_action_index >= table->action_count ||
        !eligibility->action_eligible[baseline_action_index]) {
        return OBICALL_ERR_DGT_NO_ELIGIBLE_ACTION;
    }
    double loss;
    if (obicall_dgt_action_worst_case_loss(table, baseline_action_index, &loss) != OBICALL_OK) {
        return OBICALL_ERR_DGT_NO_ELIGIBLE_ACTION;
    }
    out_decision->selected_action_id = table->actions[baseline_action_index].action_id;
    out_decision->selected_action_index = baseline_action_index;
    out_decision->worst_case_loss = loss;
    out_decision->eligible_count = 1;
    out_decision->used_fallback_baseline = 1;
    out_decision->used_hysteresis_hold = 0;
    if (hysteresis_out) {
        hysteresis_out->has_previous = 1;
        hysteresis_out->previous_action_index = baseline_action_index;
        hysteresis_out->previous_worst_case_loss = loss;
    }
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_dgt_select_action(
    const obicall_dgt_policy_table_t* table, const obicall_dgt_eligibility_t* eligibility,
    const obicall_dgt_hysteresis_state_t* hysteresis_in, double hysteresis_margin,
    uint32_t baseline_action_index, obicall_dgt_decision_t* out_decision,
    obicall_dgt_hysteresis_state_t* hysteresis_out) {
    if (!eligibility || !out_decision) return OBICALL_ERR_NULL_POINTER;
    memset(out_decision, 0, sizeof(*out_decision));
    if (hysteresis_out) memset(hysteresis_out, 0, sizeof(*hysteresis_out));

    char detail[128];
    if (obicall_dgt_validate_table(table, detail, sizeof(detail)) != OBICALL_OK) {
        return select_baseline(table, eligibility, baseline_action_index, out_decision, hysteresis_out);
    }

    uint32_t eligible_count = 0;
    uint32_t best_idx = OBICALL_DGT_INVALID_ACTION_INDEX;
    double best_loss = 0.0;

    for (uint32_t a = 0; a < table->action_count; ++a) {
        if (!eligibility->action_eligible[a]) continue;
        double loss;
        if (obicall_dgt_action_worst_case_loss(table, a, &loss) != OBICALL_OK) continue;
        eligible_count++;
        if (best_idx == OBICALL_DGT_INVALID_ACTION_INDEX || loss < best_loss ||
            (loss == best_loss && table->actions[a].action_id < table->actions[best_idx].action_id)) {
            best_idx = a;
            best_loss = loss;
        }
    }

    if (eligible_count == 0 || best_idx == OBICALL_DGT_INVALID_ACTION_INDEX) {
        return select_baseline(table, eligibility, baseline_action_index, out_decision, hysteresis_out);
    }

    uint32_t final_idx = best_idx;
    double final_loss = best_loss;
    uint32_t used_hold = 0;

    /* A previously-held action that has since become ineligible is
     * dropped unconditionally here - only an action still eligible can be
     * retained by hysteresis. */
    if (hysteresis_in && hysteresis_in->has_previous) {
        uint32_t prev = hysteresis_in->previous_action_index;
        if (prev < table->action_count && eligibility->action_eligible[prev]) {
            double prev_loss;
            if (obicall_dgt_action_worst_case_loss(table, prev, &prev_loss) == OBICALL_OK) {
                double threshold = prev_loss * (1.0 - hysteresis_margin);
                if (!(best_loss < threshold)) {
                    used_hold = (prev != best_idx) ? 1u : 0u;
                    final_idx = prev;
                    final_loss = prev_loss;
                }
            }
        }
    }

    out_decision->selected_action_id = table->actions[final_idx].action_id;
    out_decision->selected_action_index = final_idx;
    out_decision->worst_case_loss = final_loss;
    out_decision->eligible_count = eligible_count;
    out_decision->used_fallback_baseline = 0;
    out_decision->used_hysteresis_hold = used_hold;

    if (hysteresis_out) {
        hysteresis_out->has_previous = 1;
        hysteresis_out->previous_action_index = final_idx;
        hysteresis_out->previous_worst_case_loss = final_loss;
    }
    return OBICALL_OK;
}
