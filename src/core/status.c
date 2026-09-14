#include "obicall/status.h"

const char* OBICALL_CALL obicall_status_string(obicall_status_t status) {
    switch (status) {
        case OBICALL_OK: return "ok";
        case OBICALL_ERR_INVALID_ARGUMENT: return "invalid argument";
        case OBICALL_ERR_NULL_POINTER: return "null pointer";
        case OBICALL_ERR_OUT_OF_RANGE: return "value out of range";
        case OBICALL_ERR_BUFFER_TOO_SMALL: return "buffer too small";
        case OBICALL_ERR_NOT_FOUND: return "not found";
        case OBICALL_ERR_ALREADY_EXISTS: return "already exists";
        case OBICALL_ERR_UNSUPPORTED: return "unsupported";

        case OBICALL_ERR_ABI_VERSION_MISMATCH: return "ABI version mismatch";
        case OBICALL_ERR_ABI_STRUCT_SIZE_MISMATCH: return "ABI struct size mismatch";
        case OBICALL_ERR_SYMBOL_NOT_FOUND: return "symbol not found";
        case OBICALL_ERR_ARCHITECTURE_MISMATCH: return "architecture mismatch";
        case OBICALL_ERR_ARTIFACT_INTEGRITY: return "artifact integrity check failed";
        case OBICALL_ERR_DEPENDENCY_CYCLE: return "dependency cycle";
        case OBICALL_ERR_DEPENDENCY_MISSING: return "dependency missing";
        case OBICALL_ERR_CONFORMANCE_FAILED: return "conformance check failed";
        case OBICALL_ERR_LOAD_FAILED: return "load failed";

        case OBICALL_ERR_VALIDATION_NON_FINITE: return "non-finite value";
        case OBICALL_ERR_VALIDATION_DIMENSION: return "dimension mismatch";
        case OBICALL_ERR_VALIDATION_COVARIANCE: return "invalid covariance";
        case OBICALL_ERR_VALIDATION_SEQUENCE: return "invalid sequence";
        case OBICALL_ERR_VALIDATION_TIMING: return "invalid timing";
        case OBICALL_ERR_ADMISSION_DUPLICATE: return "duplicate observation";
        case OBICALL_ERR_ADMISSION_LATE: return "late observation";
        case OBICALL_ERR_ADMISSION_OVERFLOW: return "admission queue overflow";

        case OBICALL_ERR_WIRE_MALFORMED: return "malformed wire frame";
        case OBICALL_ERR_WIRE_TOO_LARGE: return "wire frame too large";
        case OBICALL_ERR_WIRE_VERSION_UNSUPPORTED: return "unsupported wire version";
        case OBICALL_ERR_WIRE_CHECKSUM: return "wire checksum mismatch";
        case OBICALL_ERR_IO: return "I/O error";
        case OBICALL_ERR_TIMEOUT: return "timeout";
        case OBICALL_ERR_AUTH: return "authentication failed";
        case OBICALL_ERR_CONNECTION_CLOSED: return "connection closed";

        case OBICALL_ERR_GATE_NOT_OWNER: return "not the current epoch owner";
        case OBICALL_ERR_GATE_STALE_EPOCH: return "stale epoch";
        case OBICALL_ERR_GATE_DUPLICATE_WINDOW: return "duplicate window";
        case OBICALL_ERR_GATE_EXPIRED: return "result expired";
        case OBICALL_ERR_GATE_INCOMPATIBLE: return "incompatible with gate state";
        case OBICALL_ERR_GATE_NOT_READY: return "not ready for promotion";
        case OBICALL_ERR_GATE_PERSISTENCE: return "gate persistence failure";

        case OBICALL_ERR_PROCESS_SPAWN: return "process spawn failed";
        case OBICALL_ERR_PROCESS_NOT_RUNNING: return "process not running";
        case OBICALL_ERR_ALREADY_RUNNING: return "already running";

        case OBICALL_ERR_DGT_NO_ELIGIBLE_ACTION: return "no eligible DGT action";
        case OBICALL_ERR_DGT_INVALID_SCORE: return "invalid DGT score";

        case OBICALL_ERR_INTERNAL: return "internal error";
        default: return "unknown status code";
    }
}
