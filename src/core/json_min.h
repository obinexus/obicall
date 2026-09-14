#ifndef OBICALL_JSON_MIN_H
#define OBICALL_JSON_MIN_H

/*
 * Minimal, bounded, non-recursive JSON reader for the fixed schemas this
 * project defines (manifests, pipeline config, DGT policy tables) - not a
 * general-purpose JSON library. No dynamic allocation; every span is a
 * (offset, length) window into the caller's buffer. Values are located by
 * scanning, not by building a DOM, which is sufficient because every
 * caller already knows the exact shape it expects.
 */

#include <stdint.h>

typedef struct json_span {
    uint32_t off;
    uint32_t len;
    int is_string; /* value looked like a "..." literal (span excludes the quotes) */
} json_span_t;

/* json/len must be a JSON object "{ ... }" (leading/trailing whitespace
 * ok). Scans members at this object's top level only (does not descend
 * into nested objects/arrays looking for the key). Returns 1 and fills
 * *out if key is found, 0 otherwise (including on a malformed object -
 * malformed input is treated as "not found", never a crash). */
int json_object_find(const uint8_t* json, uint32_t len, const char* key, json_span_t* out);

/* json/len must be a JSON array "[ ... ]". */
int json_array_count(const uint8_t* json, uint32_t len);
int json_array_get(const uint8_t* json, uint32_t len, uint32_t index, json_span_t* out);

/* Decodes a "..." string span (as returned in a json_span_t with
 * is_string=1, i.e. NOT including the surrounding quotes) into out,
 * handling \" \\ \/ \n \r \t \b \f and \uXXXX (encoded as '?' - full
 * Unicode is not needed for the identifiers/paths these schemas carry).
 * Returns the decoded length, or -1 if it would not fit in out_cap. */
int json_decode_string(const uint8_t* json, uint32_t len, char* out, uint32_t out_cap);

/* Parses a bare JSON number span (json_span_t with is_string=0) as double.
 * Returns 1 on success. */
int json_parse_number(const uint8_t* json, uint32_t len, double* out);

int json_span_is_true(const uint8_t* json, uint32_t len);
int json_span_is_false(const uint8_t* json, uint32_t len);

#endif /* OBICALL_JSON_MIN_H */
