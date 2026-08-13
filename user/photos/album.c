/* user/photos/album.c -- the other pictures next to this one.
 *
 * Opening a photo almost never means opening ONE photo. You open the one you
 * were looking for and then walk the folder it was in, and a viewer that
 * cannot do that sends you back to the file manager between every picture.
 * So the set, not the file, is the thing the app holds.
 *
 * Sorted by name, because that is the order the directory is shown in
 * everywhere else on this system and an album that walks in readdir order
 * walks differently every time the directory is rewritten.
 */
#include <dirent.h>
#include <string.h>
#include <stdio.h>

#include "photo.h"

/* Split "/data/pics/a.png" into "/data/pics" and "a.png". A path with no
 * slash is a name in the current directory, which the shell can hand us. */
static void split_path(const char *path, char *dir, size_t dcap,
                       char *name, size_t ncap)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(dir, dcap, ".");
        snprintf(name, ncap, "%s", path);
        return;
    }
    size_t dlen = (size_t)(slash - path);
    if (dlen == 0) dlen = 1;                     /* "/a.png" -> "/"          */
    if (dlen >= dcap) dlen = dcap - 1;
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';
    snprintf(name, ncap, "%s", slash + 1);
}

/* Insertion sort: the list is capped at ALBUM_MAX and typically holds tens of
 * entries, where insertion sort is faster than anything with a call stack and
 * is obviously correct at a glance. */
static void sort_names(Album *a)
{
    for (int i = 1; i < a->count; i++) {
        char key[96];
        snprintf(key, sizeof key, "%s", a->name[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(a->name[j], key) > 0) {
            memcpy(a->name[j + 1], a->name[j], sizeof a->name[0]);
            j--;
        }
        snprintf(a->name[j + 1], sizeof a->name[0], "%s", key);
    }
}

void album_open(Album *a, const char *path)
{
    char want[96];
    memset(a, 0, sizeof *a);
    a->index = -1;
    split_path(path, a->dir, sizeof a->dir, want, sizeof want);

    DIR *d = opendir(a->dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && a->count < ALBUM_MAX) {
            if (e->d_name[0] == '.') continue;             /* . .. and hidden */
            if (!photo_is_image_name(e->d_name)) continue;
            /* SKIP a name that does not fit rather than truncating it. A
             * truncated name is a path that does not resolve, so the album
             * would offer an entry that always fails to open -- which reads as
             * a decoder bug rather than as a name too long for the table. */
            if (strlen(e->d_name) >= sizeof a->name[0]) continue;
            snprintf(a->name[a->count], sizeof a->name[0], "%s", e->d_name);
            a->count++;
        }
        closedir(d);
        sort_names(a);
    }

    for (int i = 0; i < a->count; i++)
        if (strcmp(a->name[i], want) == 0) { a->index = i; break; }

    /* The file we were given might not be in the listing -- an extension we
     * do not recognise by name but decoded anyway, or a directory we cannot
     * read. Showing it alone beats refusing to show it. */
    if (a->index < 0 && a->count == 0 && want[0] != '\0') {
        snprintf(a->name[0], sizeof a->name[0], "%s", want);
        a->count = 1;
        a->index = 0;
    }
}

const char *album_path(const Album *a, int i, char *buf, size_t cap)
{
    if (i < 0 || i >= a->count) return NULL;
    /* Do not produce "//name" for a root-directory album: it resolves the same
     * on this VFS, and it looks like a bug every time it is printed. */
    if (a->dir[0] == '/' && a->dir[1] == '\0')
        snprintf(buf, cap, "/%s", a->name[i]);
    else
        snprintf(buf, cap, "%s/%s", a->dir, a->name[i]);
    return buf;
}

int album_step(Album *a, int delta)
{
    if (a->count <= 0) return -1;
    int i = a->index < 0 ? 0 : a->index + delta;
    /* Wrap. A gallery that stops dead at the last picture makes you guess
     * whether it is the end of the folder or the app having stopped. */
    while (i < 0) i += a->count;
    a->index = i % a->count;
    return a->index;
}
