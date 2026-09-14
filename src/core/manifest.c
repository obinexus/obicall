#include "obicall/manifest.h"
#include "obicall/plugin.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "json_min.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

static void set_issue(char* out, uint32_t cap, const char* fmt, const char* a) {
    if (!out || cap == 0) return;
    snprintf(out, cap, fmt, a ? a : "");
}

static uint64_t capability_flag_from_name(const char* name) {
    if (strcmp(name, "position_sensor") == 0) return OBICALL_CAP_POSITION_SENSOR;
    if (strcmp(name, "inertial_sensor") == 0) return OBICALL_CAP_INERTIAL_SENSOR;
    if (strcmp(name, "range_sensor") == 0) return OBICALL_CAP_RANGE_SENSOR;
    if (strcmp(name, "checkpoint") == 0) return OBICALL_CAP_SUPPORTS_CHECKPOINT;
    if (strcmp(name, "fault_injection") == 0) return OBICALL_CAP_SUPPORTS_FAULT_INJECTION;
    return 0;
}

static int hex_decode(const uint8_t* hex, uint32_t hex_len, uint8_t* out, uint32_t out_cap) {
    if (hex_len != out_cap * 2) return 0;
    for (uint32_t i = 0; i < out_cap; ++i) {
        uint8_t hi = hex[i * 2], lo = hex[i * 2 + 1];
        int hv, lv;
        if (hi >= '0' && hi <= '9') hv = hi - '0';
        else if (hi >= 'a' && hi <= 'f') hv = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F') hv = hi - 'A' + 10;
        else return 0;
        if (lo >= '0' && lo <= '9') lv = lo - '0';
        else if (lo >= 'a' && lo <= 'f') lv = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') lv = lo - 'A' + 10;
        else return 0;
        out[i] = (uint8_t)((hv << 4) | lv);
    }
    return 1;
}

static int find_field_str(const uint8_t* json, uint32_t len, const char* key, char* out,
                           uint32_t out_cap) {
    json_span_t sp;
    if (!json_object_find(json, len, key, &sp) || !sp.is_string) return 0;
    return json_decode_string(json + sp.off, sp.len, out, out_cap) >= 0;
}

static int find_field_num(const uint8_t* json, uint32_t len, const char* key, double* out) {
    json_span_t sp;
    if (!json_object_find(json, len, key, &sp) || sp.is_string) return 0;
    return json_parse_number(json + sp.off, sp.len, out);
}

obicall_status_t OBICALL_CALL obicall_manifest_parse_json(const uint8_t* json, uint32_t len,
                                                            obicall_manifest_t* out,
                                                            char* out_issue_detail,
                                                            uint32_t detail_cap) {
    if (!json || !out) return OBICALL_ERR_NULL_POINTER;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);

    double num;
    if (!find_field_num(json, len, "schema_version", &num)) {
        set_issue(out_issue_detail, detail_cap, "manifest missing schema_version%s", NULL);
        return OBICALL_ERR_INVALID_ARGUMENT;
    }
    out->schema_version = (uint32_t)num;
    if (out->schema_version != OBICALL_MANIFEST_SCHEMA_VERSION) {
        set_issue(out_issue_detail, detail_cap, "unsupported manifest schema_version%s", NULL);
        return OBICALL_ERR_UNSUPPORTED;
    }

    if (!find_field_str(json, len, "name", out->name, sizeof(out->name))) {
        set_issue(out_issue_detail, detail_cap, "manifest missing name%s", NULL);
        return OBICALL_ERR_INVALID_ARGUMENT;
    }
    find_field_str(json, len, "version", out->version, sizeof(out->version));

    char lang[16] = {0};
    if (find_field_str(json, len, "language", lang, sizeof(lang))) {
        if (strcmp(lang, "c") == 0) out->language = OBICALL_LANG_C;
        else if (strcmp(lang, "python") == 0) out->language = OBICALL_LANG_PYTHON;
        else {
            set_issue(out_issue_detail, detail_cap, "unknown language '%s'", lang);
            return OBICALL_ERR_UNSUPPORTED;
        }
    }

    if (find_field_num(json, len, "abi_version_major", &num)) out->abi_version_major = (uint32_t)num;
    if (find_field_num(json, len, "abi_version_minor", &num)) out->abi_version_minor = (uint32_t)num;
    find_field_str(json, len, "architecture", out->architecture, sizeof(out->architecture));
    find_field_str(json, len, "capability_slot", out->capability_slot, sizeof(out->capability_slot));
    if (find_field_num(json, len, "preference_score", &num)) out->preference_score = num;
    else out->preference_score = 0.0;

    if (!find_field_str(json, len, "artifact_path", out->artifact_path, sizeof(out->artifact_path))) {
        set_issue(out_issue_detail, detail_cap, "manifest missing artifact_path%s", NULL);
        return OBICALL_ERR_INVALID_ARGUMENT;
    }

    {
        json_span_t sp;
        char hex[2 * OBICALL_DIGEST_LEN + 1];
        if (json_object_find(json, len, "artifact_sha256", &sp) && sp.is_string) {
            int n = json_decode_string(json + sp.off, sp.len, hex, sizeof(hex));
            if (n == (int)(2 * OBICALL_DIGEST_LEN) &&
                hex_decode((const uint8_t*)hex, (uint32_t)n, out->artifact_sha256, OBICALL_DIGEST_LEN)) {
                out->has_artifact_sha256 = 1;
            } else {
                set_issue(out_issue_detail, detail_cap, "malformed artifact_sha256%s", NULL);
                return OBICALL_ERR_INVALID_ARGUMENT;
            }
        }
    }

    if (find_field_num(json, len, "config_schema_version", &num)) out->config_schema_version = (uint32_t)num;

    {
        json_span_t sp;
        if (json_object_find(json, len, "capability_flags", &sp) && !sp.is_string) {
            int n = json_array_count(json + sp.off, sp.len);
            for (int i = 0; i < n; ++i) {
                json_span_t elem;
                if (!json_array_get(json + sp.off, sp.len, (uint32_t)i, &elem) || !elem.is_string) continue;
                char name[48];
                if (json_decode_string(json + sp.off + elem.off, elem.len, name, sizeof(name)) < 0) continue;
                out->capability_flags |= capability_flag_from_name(name);
            }
        }
    }

    {
        json_span_t sp;
        if (json_object_find(json, len, "config", &sp) && !sp.is_string) {
            if (sp.len >= sizeof(out->config_json)) {
                set_issue(out_issue_detail, detail_cap, "config object too large%s", NULL);
                return OBICALL_ERR_BUFFER_TOO_SMALL;
            }
            memcpy(out->config_json, json + sp.off, sp.len);
            out->config_json[sp.len] = '\0';
        } else {
            out->config_json[0] = '{';
            out->config_json[1] = '}';
        }
    }

    {
        json_span_t sp;
        if (json_object_find(json, len, "dependencies", &sp) && !sp.is_string) {
            int n = json_array_count(json + sp.off, sp.len);
            if (n > (int)OBICALL_MAX_DEPENDENCIES) {
                set_issue(out_issue_detail, detail_cap, "too many dependencies%s", NULL);
                return OBICALL_ERR_OUT_OF_RANGE;
            }
            for (int i = 0; i < n; ++i) {
                json_span_t elem;
                if (!json_array_get(json + sp.off, sp.len, (uint32_t)i, &elem) || !elem.is_string) {
                    set_issue(out_issue_detail, detail_cap, "dependency entry must be a string%s", NULL);
                    return OBICALL_ERR_INVALID_ARGUMENT;
                }
                if (json_decode_string(json + sp.off + elem.off, elem.len,
                                        out->dependencies[out->dependency_count],
                                        OBICALL_MAX_NAME_LEN) < 0) {
                    return OBICALL_ERR_BUFFER_TOO_SMALL;
                }
                out->dependency_count++;
            }
        }
    }

    return OBICALL_OK;
}

static obicall_status_t load_one_manifest_file(const char* path, obicall_manifest_set_t* set,
                                                char* out_issue_detail, uint32_t detail_cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return OBICALL_ERR_IO;
    uint8_t buf[8192];
    size_t n = fread(buf, 1, sizeof(buf), f);
    int truncated = (fgetc(f) != EOF);
    fclose(f);
    if (truncated) {
        set_issue(out_issue_detail, detail_cap, "manifest file too large: %s", path);
        return OBICALL_ERR_BUFFER_TOO_SMALL;
    }
    if (set->count >= OBICALL_MAX_MANIFESTS) {
        set_issue(out_issue_detail, detail_cap, "too many manifests in directory%s", NULL);
        return OBICALL_ERR_OUT_OF_RANGE;
    }
    obicall_status_t st = obicall_manifest_parse_json(buf, (uint32_t)n, &set->items[set->count],
                                                        out_issue_detail, detail_cap);
    if (st == OBICALL_OK) set->count++;
    return st;
}

#if !defined(_WIN32)
static int has_suffix(const char* s, const char* suffix) {
    size_t sl = strlen(s), fl = strlen(suffix);
    return sl >= fl && strcmp(s + sl - fl, suffix) == 0;
}
#endif

obicall_status_t OBICALL_CALL obicall_manifest_load_dir(const char* dir_path,
                                                          obicall_manifest_set_t* out_set,
                                                          char* out_issue_detail,
                                                          uint32_t detail_cap) {
    if (!dir_path || !out_set) return OBICALL_ERR_NULL_POINTER;
    memset(out_set, 0, sizeof(*out_set));

#if defined(_WIN32)
    char pattern[OBICALL_MAX_PATH_LEN];
    snprintf(pattern, sizeof(pattern), "%s\\*.manifest.json", dir_path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return OBICALL_OK;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char full[OBICALL_MAX_PATH_LEN + 256];
        int written = snprintf(full, sizeof(full), "%s\\%s", dir_path, fd.cFileName);
        if (written < 0 || (size_t)written >= sizeof(full)) continue; /* path too long to hold safely */
        obicall_status_t st = load_one_manifest_file(full, out_set, out_issue_detail, detail_cap);
        if (st != OBICALL_OK) { FindClose(h); return st; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir_path);
    if (!d) return OBICALL_OK;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (!has_suffix(ent->d_name, ".manifest.json")) continue;
        char full[OBICALL_MAX_PATH_LEN + 256];
        int written = snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
        if (written < 0 || (size_t)written >= sizeof(full)) continue;
        obicall_status_t st = load_one_manifest_file(full, out_set, out_issue_detail, detail_cap);
        if (st != OBICALL_OK) { closedir(d); return st; }
    }
    closedir(d);
#endif
    return OBICALL_OK;
}

static int find_by_name(const obicall_manifest_set_t* set, const char* name) {
    for (uint32_t i = 0; i < set->count; ++i) {
        if (strcmp(set->items[i].name, name) == 0) return (int)i;
    }
    return -1;
}

obicall_status_t OBICALL_CALL obicall_manifest_resolve(const obicall_manifest_set_t* set,
                                                         const char* const* requested_names,
                                                         uint32_t requested_count,
                                                         obicall_resolution_plan_t* out_plan,
                                                         char* out_issue_detail,
                                                         uint32_t detail_cap) {
    if (!set || !requested_names || !out_plan) return OBICALL_ERR_NULL_POINTER;
    memset(out_plan, 0, sizeof(*out_plan));

    uint8_t needed[OBICALL_MAX_MANIFESTS] = {0};
    uint32_t stack[OBICALL_MAX_MANIFESTS];
    uint32_t stack_n = 0, needed_count = 0;

    for (uint32_t i = 0; i < requested_count; ++i) {
        int idx = find_by_name(set, requested_names[i]);
        if (idx < 0) {
            set_issue(out_issue_detail, detail_cap, "requested provider not found: %s", requested_names[i]);
            return OBICALL_ERR_DEPENDENCY_MISSING;
        }
        if (!needed[idx]) { needed[idx] = 1; needed_count++; stack[stack_n++] = (uint32_t)idx; }
    }

    while (stack_n > 0) {
        uint32_t idx = stack[--stack_n];
        const obicall_manifest_t* m = &set->items[idx];
        for (uint32_t d = 0; d < m->dependency_count; ++d) {
            int didx = find_by_name(set, m->dependencies[d]);
            if (didx < 0) {
                set_issue(out_issue_detail, detail_cap, "missing mandatory dependency: %s",
                          m->dependencies[d]);
                return OBICALL_ERR_DEPENDENCY_MISSING;
            }
            if (!needed[didx]) { needed[didx] = 1; needed_count++; stack[stack_n++] = (uint32_t)didx; }
        }
    }

    uint32_t indegree[OBICALL_MAX_MANIFESTS] = {0};
    for (uint32_t i = 0; i < set->count; ++i) {
        if (needed[i]) indegree[i] = set->items[i].dependency_count;
    }

    uint32_t queue[OBICALL_MAX_MANIFESTS];
    uint32_t qn = 0, qi = 0, order_n = 0;
    for (uint32_t i = 0; i < set->count; ++i) {
        if (needed[i] && indegree[i] == 0) queue[qn++] = i;
    }

    while (qi < qn) {
        uint32_t i = queue[qi++];
        out_plan->order[order_n++] = i;
        for (uint32_t j = 0; j < set->count; ++j) {
            if (!needed[j] || indegree[j] == 0) continue;
            const obicall_manifest_t* mj = &set->items[j];
            for (uint32_t d = 0; d < mj->dependency_count; ++d) {
                if (strcmp(mj->dependencies[d], set->items[i].name) == 0) {
                    if (--indegree[j] == 0) queue[qn++] = j;
                    break;
                }
            }
        }
    }

    if (order_n != needed_count) {
        char cyc[256] = {0};
        size_t off = 0;
        for (uint32_t i = 0; i < set->count && off + 1 < sizeof(cyc); ++i) {
            if (needed[i] && indegree[i] > 0) {
                int written = snprintf(cyc + off, sizeof(cyc) - off, "%s ", set->items[i].name);
                if (written > 0) off += (size_t)written;
            }
        }
        set_issue(out_issue_detail, detail_cap, "dependency cycle among: %s", cyc);
        return OBICALL_ERR_DEPENDENCY_CYCLE;
    }

    out_plan->count = order_n;
    return OBICALL_OK;
}

obicall_status_t OBICALL_CALL obicall_manifest_rank_alternatives(const obicall_manifest_set_t* set,
                                                                   const char* capability_slot,
                                                                   const char* host_architecture,
                                                                   uint32_t* out_indices,
                                                                   uint32_t out_cap,
                                                                   uint32_t* out_count) {
    if (!set || !capability_slot || !out_indices || !out_count) return OBICALL_ERR_NULL_POINTER;
    *out_count = 0;

    uint32_t candidates[OBICALL_MAX_MANIFESTS];
    uint32_t candidate_n = 0;

    for (uint32_t i = 0; i < set->count; ++i) {
        const obicall_manifest_t* m = &set->items[i];
        if (m->capability_slot[0] == '\0' || strcmp(m->capability_slot, capability_slot) != 0) continue;
        if (m->architecture[0] != '\0' && host_architecture && strcmp(m->architecture, host_architecture) != 0) {
            continue; /* architecture mismatch: ineligible, not merely deprioritized */
        }
        if (m->abi_version_major != OBICALL_ABI_VERSION_MAJOR) continue;
        if (m->abi_version_minor > OBICALL_ABI_VERSION_MINOR) continue;

        const char* req[1] = {m->name};
        obicall_resolution_plan_t plan;
        char detail[128];
        if (obicall_manifest_resolve(set, req, 1, &plan, detail, sizeof(detail)) != OBICALL_OK) {
            continue; /* unresolved mandatory dependency: ineligible */
        }

        if (candidate_n < OBICALL_MAX_MANIFESTS) candidates[candidate_n++] = i;
    }

    /* Insertion sort by ascending preference_score, ties by name - small N. */
    for (uint32_t i = 1; i < candidate_n; ++i) {
        uint32_t key = candidates[i];
        double key_score = set->items[key].preference_score;
        int j = (int)i - 1;
        while (j >= 0) {
            const obicall_manifest_t* cur = &set->items[candidates[j]];
            int should_shift = (cur->preference_score > key_score) ||
                                (cur->preference_score == key_score &&
                                 strcmp(cur->name, set->items[key].name) > 0);
            if (!should_shift) break;
            candidates[j + 1] = candidates[j];
            j--;
        }
        candidates[j + 1] = key;
    }

    uint32_t n = candidate_n < out_cap ? candidate_n : out_cap;
    for (uint32_t i = 0; i < n; ++i) out_indices[i] = candidates[i];
    *out_count = n;
    return OBICALL_OK;
}
