/* Windows compatibility shim for libsm64 */
#ifndef WIN32_COMPAT_H
#define WIN32_COMPAT_H

#ifdef _WIN32

/* Standard headers needed by compat shims */
#include <stdio.h>

/* strings.h replacement */
#include <string.h>
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif

/* dirent.h replacement - minimal implementation */
#include <io.h>
#include <stdlib.h>
#include <string.h>

struct dirent {
    char d_name[260];
};

typedef struct {
    intptr_t handle;
    struct _finddata_t data;
    struct dirent ent;
    int first;
    char path[260];
} DIR;

static inline DIR *opendir(const char *name) {
    DIR *dir = (DIR *)calloc(1, sizeof(DIR));
    if (!dir) return NULL;
    snprintf(dir->path, sizeof(dir->path), "%s\\*", name);
    dir->handle = _findfirst(dir->path, &dir->data);
    if (dir->handle == -1) {
        free(dir);
        return NULL;
    }
    dir->first = 1;
    return dir;
}

static inline struct dirent *readdir(DIR *dir) {
    if (!dir) return NULL;
    if (dir->first) {
        dir->first = 0;
    } else {
        if (_findnext(dir->handle, &dir->data) != 0)
            return NULL;
    }
    strncpy(dir->ent.d_name, dir->data.name, sizeof(dir->ent.d_name) - 1);
    dir->ent.d_name[sizeof(dir->ent.d_name) - 1] = '\0';
    return &dir->ent;
}

static inline int closedir(DIR *dir) {
    if (!dir) return -1;
    _findclose(dir->handle);
    free(dir);
    return 0;
}

#endif /* _WIN32 */
#endif /* WIN32_COMPAT_H */
