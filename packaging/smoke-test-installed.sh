#!/usr/bin/env bash
# Smoke-tests an *installed* obicall package - a .deb, an Arch Linux
# package, or the MSYS2 UCRT64 package - the way a user runs it: `obicall`
# from PATH, in a fresh scratch directory outside any source checkout, with
# PYTHONPATH and LD_LIBRARY_PATH unset so nothing but the installed files
# can satisfy the C provider, the Python adapter, or the core library.
#
# Usage: packaging/smoke-test-installed.sh [PREFIX]
#   PREFIX  install prefix of the package under test (default: /usr;
#           use "$MINGW_PREFIX" inside MSYS2 UCRT64)
#
# Beyond exit codes, `run` and `demo` are checked for evidence that both
# provider languages really worked: all four workers (C and Python, for
# brokers A and B) ran, none exited, hung, or was restarted, and
# observations from both providers' sensors reached the journal. Pass/fail
# rests only on evidence a single process writes - heartbeat files, the
# journal segment, the CLI's own JSON - never on lines in the stderr that
# every child process shares, which on Windows can interleave mid-line.
set -euo pipefail

prefix=${1:-/usr}
config="$prefix/share/obicall/examples/position-fusion.json"
recording="$prefix/share/obicall/recordings/demo.obr"

unset PYTHONPATH LD_LIBRARY_PATH

work=$(mktemp -d "${TMPDIR:-/tmp}/obicall-smoke.XXXXXX")
cd "$work"

fail() {
    echo "FAIL: $*" >&2
    echo "(evidence kept in $work)" >&2
    exit 1
}
step() { echo "==> $*"; }
expect() { # expect FILE PATTERN DESCRIPTION
    grep -aEq -- "$2" "$1" || { cat -v "$1" | tail -40 >&2; fail "$3"; }
}
reject() { # reject FILE PATTERN DESCRIPTION
    if grep -aEq -- "$2" "$1"; then grep -aE -- "$2" "$1" >&2; fail "$3"; fi
}

resolved=$(command -v obicall) || fail "obicall is not on PATH"
# Same file, not same spelling: Arch's /usr/sbin is a symlink to /usr/bin.
if ! [ "$resolved" -ef "$prefix/bin/obicall" ] && ! [ "$resolved" -ef "$prefix/bin/obicall.exe" ]; then
    fail "obicall resolves to $resolved, not the package under $prefix"
fi
step "testing $resolved from $work"

step "obicall --help"
obicall --help >help.txt || fail "--help exited nonzero"
expect help.txt '^usage: obicall' "--help printed no usage"

step "obicall doctor --json"
obicall doctor --json >doctor.json || { cat doctor.json >&2; fail "doctor exited nonzero"; }
cat doctor.json
expect doctor.json '"overall":"ok"' "doctor did not report overall ok"

step "obicall validate (installed example config)"
obicall validate --config "$config" --json >validate.json || { cat validate.json >&2; fail "validate exited nonzero"; }
cat validate.json
expect validate.json '"overall":"ok"' "validate did not report overall ok"

step "obicall replay (installed recording)"
obicall replay --input "$recording" --json >replay.json || { cat replay.json >&2; fail "replay exited nonzero"; }
cat replay.json
expect replay.json '"admitted":102,"rejected":0' "replay did not admit all 102 records"
expect replay.json '"status":"valid"' "replay produced no valid estimate"

step "obicall run, C and Python providers (6 s)"
obicall run --config "$config" --runtime-dir run --duration-seconds 6 --json >run.json 2>run.err \
    || { cat run.err >&2; fail "run exited nonzero"; }
expect run.json '"event":"started"' "run never reported started"
expect run.json '"event":"stopped"' "run never reported stopped"
# Each worker writes its own heartbeat file (atomically: temp file, then
# rename). A worker that cannot load its provider, the core library, or
# the Python adapter, or cannot reach the journal, exits - and the
# supervisor then logs the exit and restarts it, which is rejected below.
for worker in worker_position_A worker_position_B worker_inertial_A worker_inertial_B; do
    [ -s "run/$worker.pid" ] || { cat run.err >&2; fail "$worker never started (no heartbeat file)"; }
done
reject run.err 'Traceback|exited \(code=|restarting |appears hung|exceeded restart bound' \
    "a process crashed, hung, or was restarted during run"
expect run/journal.segment 'cam_position_a' "no C provider observation reached the journal"
expect run/journal.segment 'imu_a' "no Python provider observation reached the journal"
intact=$(grep -acE 'obicall-workerd\[worker_position_[AB]\]: loaded provider_c_sim .*connected to journal|obicall-worker\[python:worker_inertial_[AB]\]: connected' run.err || true)
echo "all four workers (C and Python, brokers A and B) ran the whole time; both sensors present in the journal"
echo "($intact of 4 worker connection log lines intact in the shared stderr - informational)"

step "obicall demo --scenario broker-failover"
obicall demo --scenario broker-failover --config "$config" --json >demo.json 2>demo.err \
    || { cat demo.json demo.err >&2; fail "demo exited nonzero"; }
cat demo.json
expect demo.json '"overall":"pass"' "broker-failover demo did not pass"
demo_journal=$(ls obicall-demo-run-*/journal.segment)
expect "$demo_journal" 'imu_a' "no Python provider observation reached the journal during the demo"

cd /
rm -rf "$work"
echo "PASS: installed obicall smoke test ($resolved)"
