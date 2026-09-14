# Dimensional Game Theory: what's actually implemented

`Nnamdi Michael Okpala's "Dimensional Game Theory: A Framework for Strategic
Algorithm Development"` (OBINexus Computing, 22 May 2025, supplied as
`ReADME.md` with this brief) proposes multidimensional strategies and
adaptive weighting as a general framework. This document is about the much
narrower thing actually built in `src/core/dgt.c`: a versioned,
eligibility-filtered, minimax action selector over a small fixed action and
scenario catalog. Where the two diverge, it's deliberate — see "Research
boundaries" below.

## What DGT is, here

- **Actions** are approved combinations of (sensor subset, estimator mode,
  provider) — concretely, in the shipped example pipeline, which subset of
  `{position sensor, inertial sensor}` currently feeds the Kalman estimator
  (`obicall_dgt_action_t.sensor_subset_mask`, `src/broker/broker_main.c`).
- **Scenarios** are a fixed catalog of fault models — dropout, bias,
  processing delay — used only to compute a worst-case loss per action for
  *ranking* purposes. Sensors are not modeled as strategic agents; nothing
  here treats a sensor's behavior as adversarial optimization.
- **Cost** for one (action, scenario) pair is four nonnegative,
  independently-weighted, independently-scaled components:
  `estimation_error`, `delay`, `coverage_loss`, `resource_cost`
  (`obicall_dgt_cost_components_t`). `obicall_dgt_action_worst_case_loss`
  computes `max` over scenarios of the weighted, scale-normalized sum.
- **Selection** (`obicall_dgt_select_action`) is: eliminate ineligible
  actions first (freshness/observability/capability/ownership constraints —
  computed by the caller, e.g. `src/broker/broker_main.c` marks an action
  ineligible if any sensor its mask requires hasn't been seen recently
  enough), then pick the eligible action with the lowest worst-case loss,
  breaking exact ties by the lowest `action_id`.
- **Hysteresis** retains the previously-selected action unless a candidate
  beats it by more than a configured fractional margin — *except* that a
  previously-held action which has since become ineligible is dropped
  unconditionally; hysteresis never retains unsafe eligibility.
- **Fallback**: an invalid policy table (fails `obicall_dgt_validate_table` —
  negative weight, negative cost, zero scale, or no positive weight at all)
  falls back to a caller-supplied baseline action index, itself still
  subject to the eligibility check. If even the baseline is ineligible, the
  function returns `OBICALL_ERR_DGT_NO_ELIGIBLE_ACTION` and the caller must
  report invalid output — DGT never invents an answer when it has no
  eligible one.

**DGT cannot grant gate ownership, weaken observation validation, or invent
estimator covariance.** This isn't a documented promise layered on top of
unrelated code — it's a fact about the call graph: `src/core/dgt.c` has no
reference to `obicall_gate_state_t`, never calls
`obicall_observation_validate`, and never writes an `obicall_result_t`'s
`covariance` field (the estimator does that, and the estimator calls DGT for
which sensors to use, not the other way around).

## The action actually changes

The specific, checkable claim the brief asks for: DGT's decision changes an
actual eligible action in the running system, not just a logged label.
`src/broker/broker_main.c`'s main loop computes `selected_mask` from
`obicall_dgt_select_action`'s output, and an admitted observation is only
ever passed to `obicall_kalman_update_position` if its sensor's bit is set
in that mask:

```c
if (e->admitted && (selected_mask & sensor_bit(e->obs.sensor_id)) &&
    e->obs.payload_shape == OBICALL_SHAPE_POSITION_2D) {
    ... obicall_kalman_update_position(...) ...
}
```

An observation from a sensor outside the selected subset is still admitted
at the journal (recorded, not discarded) but is not fused. When a fault
injected into the C reference provider (`$ctrl:fault` sentinel — see
"Fault injection" in the provider's own doc comment,
`src/providers/c_sim/provider_c_sim.c`) makes a sensor stop reporting, that
sensor's bit becomes unobservable, which makes every action requiring it
ineligible, which forces the selector onto whatever remains eligible — a
real, observable change in which sensor data the estimator actually uses.

## Required synthetic fixture

The brief specifies an exact fixture: three actions (camera-heavy,
inertial-heavy, balanced) with worst-case losses of **0.85, 0.70, 0.35**,
where the balanced (third) action must win when eligible and must not win
once it isn't. `tests/unit/test_dgt.c::make_fixture_table` builds exactly
this (one scenario, weight 1 on `estimation_error` only, all scales 1, so
each action's worst-case loss reduces to its own `estimation_error` value —
chosen deliberately so the fixture's numbers are exact, not approximate):

```c
t->cost[0][0].estimation_error = 0.85;  // camera_heavy
t->cost[1][0].estimation_error = 0.70;  // inertial_heavy
t->cost[2][0].estimation_error = 0.35;  // balanced
```

`test_fixture_losses_are_exact` checks all three losses to `1e-12`.
`test_balanced_action_wins_when_eligible` checks action index 2 is selected.
`test_balanced_action_excluded_when_ineligible` marks it ineligible and
checks the selector falls back to index 1 (the next-best *eligible* action,
0.70) — never index 2. These are labeled synthetic in both the test file and
here, per the brief's own instruction to label synthetic policy inputs as
synthetic.

## The live pipeline's own table

Separately, `src/broker/broker_main.c::init_default_dgt_table` defines a
**different**, also-synthetic table sized for the shipped example pipeline's
actual two sensors (position, inertial) and four scenarios (nominal,
position-dropout, inertial-bias, processing-delay). Its numbers were chosen
by hand to be a plausible, self-consistent illustration — e.g. an
inertial-only action's cost spikes under an inertial-bias scenario, a
position-only action's cost spikes under position-dropout, and the balanced
action degrades more gracefully under either single-sensor fault while
having the lowest cost when both sensors are healthy — and are explicitly
**not** measurements from any real hardware or simulation campaign. This is
a second, independent illustration of the same eligibility-then-minimax
mechanism the required fixture tests, sized to actually drive the demo
pipeline rather than to hit three exact numbers.

## Research boundaries

The manuscript's claim that "perfect play across all dimensions yields a
deterministic tie" in a zero-sum game does not hold as a general premise —
even the trivial constant-payoff zero-sum game (+1/−1 regardless of actions)
is zero-sum, and its unique Nash equilibrium is not a tie. Nash equilibrium
describes resistance to unilateral deviation, not a guarantee of equal
outcomes. Nothing in this implementation relies on that claim: the selector
here is a **restricted minimax policy over a fixed, small action and
scenario catalog**, not a general-sum, general-agent equilibrium solver, and
it makes no equilibrium claim at all — it just picks the eligible action
with the lowest worst-case cost against a fixed fault catalog.

Costs are estimated from simulation and recorded data, or in this prototype,
from hand-authored illustrative tables — never implied to be directly
observable ground-truth error at runtime. Online decisions
(`obicall_dgt_select_action`'s inputs: current eligibility and the static
table) never depend on hidden simulation ground truth; the simulated
provider's true trajectory is not and cannot be read by the policy.

## What's not implemented

A real evaluation of DGT against held-out fault scenarios, compared to a
fixed-policy baseline, with policy-switch frequency and computational
overhead recorded alongside accuracy, is not implemented — there is no
scenario-generation or evaluation harness beyond the unit fixtures and the
live demo's single fault-injection path. "Improved loss on a training
scenario table is insufficient evidence of general reliability" is true here
by omission: no such evidence, improved or otherwise, is claimed.
