#include "json_min.h"

#include <string.h>
#include <stdlib.h>

static int is_ws(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static int skip_string(const uint8_t* buf, uint32_t len, uint32_t pos) {
    if (pos >= len || buf[pos] != '"') return -1;
    pos++;
    while (pos < len) {
        uint8_t ch = buf[pos];
        if (ch == '"') return (int)(pos + 1);
        if (ch == '\\') {
            if (pos + 1 >= len) return -1;
            if (buf[pos + 1] == 'u') {
                if (pos + 6 > len) return -1;
                pos += 6;
            } else {
                pos += 2;
            }
            continue;
        }
        pos++;
    }
    return -1;
}

/* Returns the offset one past the end of the value starting at/after pos
 * (leading whitespace is skipped), or -1 if malformed. *is_string is set
 * to 1 iff the value is a "..." literal. */
static int scan_value(const uint8_t* buf, uint32_t len, uint32_t pos, int* is_string) {
    while (pos < len && is_ws(buf[pos])) pos++;
    if (pos >= len) return -1;
    *is_string = 0;
    uint8_t ch = buf[pos];

    if (ch == '"') {
        *is_string = 1;
        return skip_string(buf, len, pos);
    }
    if (ch == '{' || ch == '[') {
        uint8_t open = ch, close = (ch == '{') ? '}' : ']';
        int depth = 1;
        pos++;
        while (pos < len && depth > 0) {
            uint8_t c = buf[pos];
            if (c == '"') {
                int r = skip_string(buf, len, pos);
                if (r < 0) return -1;
                pos = (uint32_t)r;
                continue;
            }
            if (c == open) depth++;
            else if (c == close) depth--;
            pos++;
        }
        return (depth == 0) ? (int)pos : -1;
    }
    if (ch == 't') {
        return (pos + 4 <= len && memcmp(buf + pos, "true", 4) == 0) ? (int)(pos + 4) : -1;
    }
    if (ch == 'f') {
        return (pos + 5 <= len && memcmp(buf + pos, "false", 5) == 0) ? (int)(pos + 5) : -1;
    }
    if (ch == 'n') {
        return (pos + 4 <= len && memcmp(buf + pos, "null", 4) == 0) ? (int)(pos + 4) : -1;
    }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        uint32_t start = pos;
        if (buf[pos] == '-') pos++;
        while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') pos++;
        if (pos < len && buf[pos] == '.') {
            pos++;
            while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') pos++;
        }
        if (pos < len && (buf[pos] == 'e' || buf[pos] == 'E')) {
            pos++;
            if (pos < len && (buf[pos] == '+' || buf[pos] == '-')) pos++;
            while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') pos++;
        }
        return (pos == start) ? -1 : (int)pos;
    }
    return -1;
}

static void span_from_value(const uint8_t* json, uint32_t value_start, int value_end, int is_str,
                             json_span_t* out) {
    if (is_str) {
        out->off = value_start + 1;
        out->len = (uint32_t)value_end - 1 - out->off;
    } else {
        out->off = value_start;
        out->len = (uint32_t)value_end - value_start;
    }
    out->is_string = is_str;
    (void)json;
}

int json_object_find(const uint8_t* json, uint32_t len, const char* key, json_span_t* out) {
    uint32_t pos = 0;
    while (pos < len && is_ws(json[pos])) pos++;
    if (pos >= len || json[pos] != '{') return 0;
    pos++;
    size_t key_len = strlen(key);

    for (;;) {
        while (pos < len && is_ws(json[pos])) pos++;
        if (pos >= len || json[pos] == '}') return 0;
        if (json[pos] != '"') return 0;
        int key_end = skip_string(json, len, pos);
        if (key_end < 0) return 0;
        uint32_t key_start = pos + 1;
        uint32_t key_str_len = (uint32_t)key_end - 1 - key_start;
        pos = (uint32_t)key_end;

        while (pos < len && is_ws(json[pos])) pos++;
        if (pos >= len || json[pos] != ':') return 0;
        pos++;
        while (pos < len && is_ws(json[pos])) pos++;

        uint32_t value_start = pos;
        int is_str = 0;
        int value_end = scan_value(json, len, pos, &is_str);
        if (value_end < 0) return 0;

        int match = (key_str_len == key_len) && (memcmp(json + key_start, key, key_len) == 0);
        if (match) {
            span_from_value(json, value_start, value_end, is_str, out);
            return 1;
        }

        pos = (uint32_t)value_end;
        while (pos < len && is_ws(json[pos])) pos++;
        if (pos < len && json[pos] == ',') { pos++; continue; }
        if (pos < len && json[pos] == '}') return 0;
        return 0;
    }
}

static int array_open(const uint8_t* json, uint32_t len, uint32_t* pos_out) {
    uint32_t pos = 0;
    while (pos < len && is_ws(json[pos])) pos++;
    if (pos >= len || json[pos] != '[') return 0;
    *pos_out = pos + 1;
    return 1;
}

int json_array_count(const uint8_t* json, uint32_t len) {
    uint32_t pos;
    if (!array_open(json, len, &pos)) return -1;
    int count = 0;
    for (;;) {
        while (pos < len && is_ws(json[pos])) pos++;
        if (pos >= len) return -1;
        if (json[pos] == ']') return count;
        int is_str = 0;
        int end = scan_value(json, len, pos, &is_str);
        if (end < 0) return -1;
        count++;
        pos = (uint32_t)end;
        while (pos < len && is_ws(json[pos])) pos++;
        if (pos < len && json[pos] == ',') { pos++; continue; }
        if (pos < len && json[pos] == ']') return count;
        return -1;
    }
}

int json_array_get(const uint8_t* json, uint32_t len, uint32_t index, json_span_t* out) {
    uint32_t pos;
    if (!array_open(json, len, &pos)) return 0;
    uint32_t i = 0;
    for (;;) {
        while (pos < len && is_ws(json[pos])) pos++;
        if (pos >= len || json[pos] == ']') return 0;
        uint32_t value_start = pos;
        int is_str = 0;
        int end = scan_value(json, len, pos, &is_str);
        if (end < 0) return 0;
        if (i == index) {
            span_from_value(json, value_start, end, is_str, out);
            return 1;
        }
        i++;
        pos = (uint32_t)end;
        while (pos < len && is_ws(json[pos])) pos++;
        if (pos < len && json[pos] == ',') { pos++; continue; }
        return 0;
    }
}

int json_decode_string(const uint8_t* json, uint32_t len, char* out, uint32_t out_cap) {
    uint32_t oi = 0, i = 0;
    while (i < len) {
        uint8_t ch = json[i];
        if (ch == '\\') {
            if (i + 1 >= len) return -1;
            uint8_t esc = json[i + 1];
            if (esc == 'u') {
                if (i + 6 > len || oi >= out_cap) return -1;
                out[oi++] = '?';
                i += 6;
                continue;
            }
            char decoded;
            switch (esc) {
                case '"': decoded = '"'; break;
                case '\\': decoded = '\\'; break;
                case '/': decoded = '/'; break;
                case 'n': decoded = '\n'; break;
                case 't': decoded = '\t'; break;
                case 'r': decoded = '\r'; break;
                case 'b': decoded = '\b'; break;
                case 'f': decoded = '\f'; break;
                default: return -1;
            }
            if (oi >= out_cap) return -1;
            out[oi++] = decoded;
            i += 2;
            continue;
        }
        if (oi >= out_cap) return -1;
        out[oi++] = (char)ch;
        i++;
    }
    if (oi >= out_cap) return -1;
    out[oi] = '\0';
    return (int)oi;
}

int json_parse_number(const uint8_t* json, uint32_t len, double* out) {
    char buf[64];
    if (len >= sizeof(buf)) return 0;
    memcpy(buf, json, len);
    buf[len] = '\0';
    char* endptr = NULL;
    double v = strtod(buf, &endptr);
    if (endptr == buf) return 0;
    *out = v;
    return 1;
}

int json_span_is_true(const uint8_t* json, uint32_t len) {
    return len == 4 && memcmp(json, "true", 4) == 0;
}
int json_span_is_false(const uint8_t* json, uint32_t len) {
    return len == 5 && memcmp(json, "false", 5) == 0;
}
