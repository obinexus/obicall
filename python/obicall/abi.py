"""ctypes bindings against libobicall's public C ABI (include/obicall/*.h).

Every struct here mirrors a real C struct field-for-field (see the header
cited above each class) and every function prototype below sets explicit
argtypes/restype before the function is ever called, per docs/ABI.md
"Python adapter". This module does not reimplement wire encoding, CRC32,
or validation in Python - it calls the real compiled functions in
libobicall so a layout or logic mistake here surfaces as a decode failure
on the C side, not as two silently-diverging implementations.
"""

import ctypes as C
import os

OBICALL_SENSOR_ID_LEN = 32
OBICALL_PIPELINE_ID_LEN = 32
OBICALL_MAX_PAYLOAD_DOUBLES = 6
OBICALL_MAX_COV_DOUBLES = 36
OBICALL_MAX_SOURCES = 8
OBICALL_DIGEST_LEN = 32
OBICALL_RUN_TOKEN_LEN = 32
OBICALL_WIRE_HEADER_LEN = 16
OBICALL_WIRE_VERSION = 1
OBICALL_OBSERVATION_SCHEMA_VERSION = 1

OBICALL_CLOCK_DOMAIN_LOCAL_MONOTONIC = 1
OBICALL_FRAME_LOCAL_ENU = 1
OBICALL_UNITS_METERS = 1
OBICALL_SHAPE_POSITION_2D = 2

OBICALL_ROLE_WORKER = 1
OBICALL_ROLE_BROKER = 2
OBICALL_ROLE_GATE = 3
OBICALL_ROLE_JOURNAL = 4
OBICALL_ROLE_CLI = 5

OBICALL_MSG_AUTH_HELLO = 1
OBICALL_MSG_HEARTBEAT = 2
OBICALL_MSG_OBSERVATION_SUBMIT = 3
OBICALL_MSG_EVENT = 4
OBICALL_MSG_ADMISSION_DECISION = 5
OBICALL_MSG_RESULT_PUBLISH = 6
OBICALL_MSG_RESULT_ACK = 7


class ObicallObservation(C.Structure):
    """Mirrors obicall_observation_t (include/obicall/observation.h)."""

    _fields_ = [
        ("struct_size", C.c_uint32),
        ("schema_version", C.c_uint32),
        ("sensor_id", C.c_char * OBICALL_SENSOR_ID_LEN),
        ("source_boot_id", C.c_uint64),
        ("sequence", C.c_uint64),
        ("sample_time_ns", C.c_int64),
        ("arrival_time_ns", C.c_int64),
        ("clock_domain", C.c_uint32),
        ("time_uncertainty_s", C.c_double),
        ("coordinate_frame", C.c_uint32),
        ("units", C.c_uint32),
        ("calibration_version", C.c_uint32),
        ("payload_shape", C.c_uint32),
        ("payload_count", C.c_uint32),
        ("payload", C.c_double * OBICALL_MAX_PAYLOAD_DOUBLES),
        ("covariance_count", C.c_uint32),
        ("covariance", C.c_double * OBICALL_MAX_COV_DOUBLES),
    ]


class ObicallWireHeader(C.Structure):
    """Mirrors obicall_wire_header_t (include/obicall/wire.h)."""

    _fields_ = [
        ("wire_version", C.c_uint8),
        ("msg_type", C.c_uint8),
        ("flags", C.c_uint16),
        ("payload_len", C.c_uint32),
        ("crc32", C.c_uint32),
    ]


class ObicallAuthHello(C.Structure):
    """Mirrors obicall_msg_auth_hello_t (include/obicall/wire.h)."""

    _fields_ = [
        ("protocol_version", C.c_uint32),
        ("role", C.c_uint32),
        ("run_token", C.c_uint8 * OBICALL_RUN_TOKEN_LEN),
    ]


ValidationCallbackType = C.CFUNCTYPE(None, C.c_void_p, C.c_int)


def load_library(path: str):
    """Loads libobicall and declares every function this module uses.

    Declaring argtypes/restype up front (rather than relying on ctypes'
    error-prone int-by-default guessing) is what makes this a real,
    checked FFI binding rather than a hopeful one.
    """
    if not os.path.isfile(path):
        raise FileNotFoundError(f"libobicall not found at {path}")
    lib = C.CDLL(path)

    lib.obicall_crc32.argtypes = [C.c_char_p, C.c_uint32]
    lib.obicall_crc32.restype = C.c_uint32

    lib.obicall_sha256.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p]
    lib.obicall_sha256.restype = None

    lib.obicall_wire_encode_header.argtypes = [C.POINTER(ObicallWireHeader), C.c_char_p]
    lib.obicall_wire_encode_header.restype = C.c_int32

    lib.obicall_wire_decode_header.argtypes = [C.c_char_p, C.POINTER(ObicallWireHeader)]
    lib.obicall_wire_decode_header.restype = C.c_int32

    lib.obicall_wire_encode_auth_hello.argtypes = [
        C.POINTER(ObicallAuthHello), C.c_char_p, C.c_uint32, C.POINTER(C.c_uint32)
    ]
    lib.obicall_wire_encode_auth_hello.restype = C.c_int32

    lib.obicall_wire_encode_observation.argtypes = [
        C.POINTER(ObicallObservation), C.c_char_p, C.c_uint32, C.POINTER(C.c_uint32)
    ]
    lib.obicall_wire_encode_observation.restype = C.c_int32

    lib.obicall_wire_decode_observation.argtypes = [C.c_char_p, C.c_uint32, C.POINTER(ObicallObservation)]
    lib.obicall_wire_decode_observation.restype = C.c_int32

    lib.obicall_observation_validate.argtypes = [C.POINTER(ObicallObservation), ValidationCallbackType, C.c_void_p]
    lib.obicall_observation_validate.restype = C.c_int32

    lib.obicall_status_string.argtypes = [C.c_int32]
    lib.obicall_status_string.restype = C.c_char_p

    lib.obicall_version_string.argtypes = []
    lib.obicall_version_string.restype = C.c_char_p

    return lib


class ObservationValidator:
    """Wraps obicall_observation_validate. The ctypes callback is created
    once in __init__ and kept as an instance attribute for the object's
    whole lifetime (not re-created per call and not left as a call-site
    temporary) - the pattern ctypes' own docs recommend so the callback
    trampoline can never be garbage-collected while C might still invoke
    it, even though this particular call is synchronous."""

    def __init__(self, lib):
        self._lib = lib
        self._issues = []
        self._callback = ValidationCallbackType(self._on_issue)

    def _on_issue(self, _user_data, issue):
        self._issues.append(int(issue))

    def validate(self, obs: ObicallObservation):
        self._issues = []
        status = self._lib.obicall_observation_validate(C.byref(obs), self._callback, None)
        return int(status), list(self._issues)


def encode_observation(lib, obs: ObicallObservation) -> bytes:
    buf = C.create_string_buffer(1200)
    out_len = C.c_uint32(0)
    status = lib.obicall_wire_encode_observation(C.byref(obs), buf, len(buf), C.byref(out_len))
    if status != 0:
        raise RuntimeError(f"obicall_wire_encode_observation failed: {lib.obicall_status_string(status).decode()}")
    return buf.raw[: out_len.value]


def encode_auth_hello(lib, token: bytes, role: int) -> bytes:
    hello = ObicallAuthHello()
    hello.protocol_version = OBICALL_WIRE_VERSION
    hello.role = role
    hello.run_token = (C.c_uint8 * OBICALL_RUN_TOKEN_LEN)(*token)
    buf = C.create_string_buffer(64)
    out_len = C.c_uint32(0)
    status = lib.obicall_wire_encode_auth_hello(C.byref(hello), buf, len(buf), C.byref(out_len))
    if status != 0:
        raise RuntimeError("obicall_wire_encode_auth_hello failed")
    return buf.raw[: out_len.value]


def send_frame(lib, sock, msg_type: int, payload: bytes):
    crc = lib.obicall_crc32(payload, len(payload))
    hdr = ObicallWireHeader()
    hdr.wire_version = OBICALL_WIRE_VERSION
    hdr.msg_type = msg_type
    hdr.flags = 0
    hdr.payload_len = len(payload)
    hdr.crc32 = crc
    out = C.create_string_buffer(OBICALL_WIRE_HEADER_LEN)
    status = lib.obicall_wire_encode_header(C.byref(hdr), out)
    if status != 0:
        raise RuntimeError("obicall_wire_encode_header failed")
    sock.sendall(out.raw[:OBICALL_WIRE_HEADER_LEN] + payload)
