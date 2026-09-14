#include "osal.h"

#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32)

#include <windows.h>

struct osal_module {
    HMODULE h;
};

int osal_dynload_open(const char* path, osal_module_t** out_mod) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return -1;
    wchar_t* wpath = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!wpath) return -1;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    /* Resolve to an absolute path first: combining a relative path with
     * the explicit LOAD_LIBRARY_SEARCH_* flags below is not guaranteed to
     * behave the same as the legacy default search order, so remove the
     * ambiguity rather than rely on it. */
    wchar_t full_path[2048];
    DWORD full_len = GetFullPathNameW(wpath, 2048, full_path, NULL);
    free(wpath);
    if (full_len == 0 || full_len >= 2048) return -1;

    /* LOAD_LIBRARY_SEARCH_DEFAULT_DIRS covers the application directory,
     * system directories, and any AddDllDirectory-registered paths;
     * LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR additionally covers this DLL's own
     * directory (for its own transitive dependencies) - together neither
     * the process's current directory nor PATH are consulted, closing
     * the classic DLL-planting search-path attack. */
    HMODULE h = LoadLibraryExW(full_path, NULL,
                                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!h) return -1;
    osal_module_t* m = (osal_module_t*)malloc(sizeof(*m));
    if (!m) { FreeLibrary(h); return -1; }
    m->h = h;
    *out_mod = m;
    return 0;
}

void* osal_dynload_symbol(osal_module_t* mod, const char* name) {
    return (void*)(intptr_t)GetProcAddress(mod->h, name);
}

void osal_dynload_close(osal_module_t* mod) {
    if (!mod) return;
    FreeLibrary(mod->h);
    free(mod);
}

const char* osal_dynload_last_error(void) {
    static char buf[128];
    snprintf(buf, sizeof(buf), "Win32 error %lu", (unsigned long)GetLastError());
    return buf;
}

#else

#include <dlfcn.h>

struct osal_module {
    void* h;
};

int osal_dynload_open(const char* path, osal_module_t** out_mod) {
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) return -1;
    osal_module_t* m = (osal_module_t*)malloc(sizeof(*m));
    if (!m) { dlclose(h); return -1; }
    m->h = h;
    *out_mod = m;
    return 0;
}

void* osal_dynload_symbol(osal_module_t* mod, const char* name) { return dlsym(mod->h, name); }

void osal_dynload_close(osal_module_t* mod) {
    if (!mod) return;
    dlclose(mod->h);
    free(mod);
}

const char* osal_dynload_last_error(void) {
    const char* e = dlerror();
    return e ? e : "unknown dynamic loader error";
}

#endif
