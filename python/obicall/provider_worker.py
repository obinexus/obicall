#!/usr/bin/env python3
"""obicall python worker: the Python language adapter's provider process.

Spawned directly by the supervisor (src/supervisor/supervisor.c) when a
provider manifest declares "language": "python", in place of the native
obicall-workerd binary. Simulates an inertial/dead-reckoning position
source and submits it to the journal exactly like the C worker does,
using the real compiled libobicall for validation and wire encoding (see
obicall.abi) rather than a second, Python-only implementation of either.
"""

import argparse
import ctypes
import json
import os
import random
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from obicall import abi  # noqa: E402


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--journal-port", type=int, required=True)
    ap.add_argument("--token", required=True)
    ap.add_argument("--runtime-dir", required=True)
    ap.add_argument("--worker-name", required=True)
    ap.add_argument("--instance-seed", type=int, default=1)
    ap.add_argument("--core-lib", required=True)
    return ap.parse_args()


def write_heartbeat(runtime_dir, worker_name):
    path = os.path.join(runtime_dir, f"{worker_name}.pid")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="ascii") as f:
        f.write(str(os.getpid()))
    os.replace(tmp, path)


def main():
    args = parse_args()
    lib = abi.load_library(args.core_lib)

    with open(args.manifest, "r", encoding="utf-8") as f:
        manifest = json.load(f)
    cfg = manifest.get("config", {})

    sensor_id = cfg.get("sensor_id", "imu_a").encode("ascii")[: abi.OBICALL_SENSOR_ID_LEN - 1]
    noise_std = float(cfg.get("noise_std_m", 0.5))
    update_period_s = float(cfg.get("update_period_ms", 100)) / 1000.0
    origin_x = float(cfg.get("origin_x", 0.0))
    origin_y = float(cfg.get("origin_y", 0.0))
    speed_x = float(cfg.get("speed_x", 1.0))
    speed_y = float(cfg.get("speed_y", 0.4))

    token = bytes.fromhex(args.token)
    if len(token) != abi.OBICALL_RUN_TOKEN_LEN:
        print(f"obicall-worker[python]: malformed token ({len(token)} bytes)", file=sys.stderr)
        return 2

    rng = random.Random(args.instance_seed)
    source_boot_id = (time.time_ns() ^ args.instance_seed) & 0xFFFFFFFFFFFFFFFF

    # Written before connecting, not after: interpreter startup plus a
    # slow/retried connect can otherwise look identical to a genuine hang
    # to the supervisor's heartbeat-file staleness check.
    write_heartbeat(args.runtime_dir, args.worker_name)

    try:
        sock = socket.create_connection(("127.0.0.1", args.journal_port), timeout=3.0)
    except OSError as exc:
        print(f"obicall-worker[python]: failed to connect to journal: {exc}", file=sys.stderr)
        return 3

    abi.send_frame(lib, sock, abi.OBICALL_MSG_AUTH_HELLO, abi.encode_auth_hello(lib, token, abi.OBICALL_ROLE_WORKER))

    validator = abi.ObservationValidator(lib)
    seq = 1
    start = time.monotonic_ns()
    last_heartbeat = 0

    print(
        f"obicall-worker[python:{args.worker_name}]: connected, sensor_id={sensor_id.decode()}, "
        f"obicall core {lib.obicall_version_string().decode()}",
        file=sys.stderr,
    )

    while True:
        now = time.monotonic_ns()
        t = (now - start) / 1e9

        zx = origin_x + speed_x * t + rng.gauss(0.0, noise_std)
        zy = origin_y + speed_y * t + rng.gauss(0.0, noise_std)

        obs = abi.ObicallObservation()
        obs.struct_size = ctypes.sizeof(abi.ObicallObservation)
        obs.schema_version = abi.OBICALL_OBSERVATION_SCHEMA_VERSION
        obs.sensor_id = sensor_id
        obs.source_boot_id = source_boot_id
        obs.sequence = seq
        obs.sample_time_ns = now
        obs.arrival_time_ns = now
        obs.clock_domain = abi.OBICALL_CLOCK_DOMAIN_LOCAL_MONOTONIC
        obs.time_uncertainty_s = 0.02
        obs.coordinate_frame = abi.OBICALL_FRAME_LOCAL_ENU
        obs.units = abi.OBICALL_UNITS_METERS
        obs.calibration_version = 1
        obs.payload_shape = abi.OBICALL_SHAPE_POSITION_2D
        obs.payload_count = 2
        obs.payload[0] = zx
        obs.payload[1] = zy
        obs.covariance_count = 4
        var = noise_std * noise_std
        obs.covariance[0] = var
        obs.covariance[1] = 0.0
        obs.covariance[2] = 0.0
        obs.covariance[3] = var

        status, issues = validator.validate(obs)
        if status != 0:
            print(f"obicall-worker[python]: local validation rejected observation: issues={issues}", file=sys.stderr)
        else:
            wire_bytes = abi.encode_observation(lib, obs)
            try:
                abi.send_frame(lib, sock, abi.OBICALL_MSG_OBSERVATION_SUBMIT, wire_bytes)
                seq += 1
            except OSError as exc:
                print(f"obicall-worker[python]: journal connection lost: {exc}", file=sys.stderr)
                return 4

        if now - last_heartbeat > 200_000_000:
            write_heartbeat(args.runtime_dir, args.worker_name)
            last_heartbeat = now

        time.sleep(update_period_s)


if __name__ == "__main__":
    raise SystemExit(main())
