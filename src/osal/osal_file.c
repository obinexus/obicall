#include "osal.h"

#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)

#include <windows.h>
#include <direct.h>

int osal_file_atomic_replace(const char* tmp_path, const char* final_path) {
    wchar_t wtmp[1024], wfinal[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, tmp_path, -1, wtmp, 1024) <= 0) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, final_path, -1, wfinal, 1024) <= 0) return -1;
    return MoveFileExW(wtmp, wfinal, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}

int osal_mkdir_p(const char* path) {
    char buf[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '\\' || buf[i] == '/') {
            char save = buf[i];
            buf[i] = '\0';
            if (buf[1] != ':' || i > 2) { /* skip bare "C:" drive prefix */
                _mkdir(buf);
            }
            buf[i] = save;
        }
    }
    _mkdir(buf); /* final component; EEXIST (already created) is not an error here */
    return 0;
}

int64_t osal_file_mtime_ns(const char* path) {
    wchar_t wpath[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) <= 0) return -1;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &attr)) return -1;
    ULARGE_INTEGER u;
    u.LowPart = attr.ftLastWriteTime.dwLowDateTime;
    u.HighPart = attr.ftLastWriteTime.dwHighDateTime;
    const uint64_t epoch_diff_100ns = 116444736000000000ULL;
    if (u.QuadPart < epoch_diff_100ns) return 0;
    return (int64_t)((u.QuadPart - epoch_diff_100ns) * 100ULL);
}

#else

#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

int osal_file_atomic_replace(const char* tmp_path, const char* final_path) {
    if (rename(tmp_path, final_path) != 0) return -1;
    /* fsync the containing directory so the rename itself is durable, not
     * just the file's own contents. */
    char dir[1024];
    strncpy(dir, final_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char* slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        int fd = open(dir, O_RDONLY);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
    return 0;
}

int osal_mkdir_p(const char* path) {
    char buf[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            mkdir(buf, 0755);
            buf[i] = '/';
        }
    }
    mkdir(buf, 0755);
    return 0;
}

int64_t osal_file_mtime_ns(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime * 1000000000LL;
}

#endif
