/* user/bin/files.c -- a file manager: browse the filesystem, see sizes, open
 * files in the Editor.
 *
 * Reads directories with opendir/readdir, sizes/types with stat, sorts folders
 * first, and on a file tap launches /data/apps/edit/edit.elf with
 * EDIT_FILE=<full path> in its environment (edit.c reads that). Folders drill
 * in; "Up" climbs. A concrete, usable window onto EMBKFS + /system + /data --
 * and the click-to-open half of a real create/browse/edit/save loop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>

#include "embk.h"
#include "ui.h"
#include "em.h"
#include "theme.h"

#define MAX_ENTRIES 256
#define NAME_LEN    112

struct entry { char name[NAME_LEN]; bool is_dir; long size; };

static struct entry g_entries[MAX_ENTRIES];
static int   g_count = 0;
static char  g_cwd[512] = "/";
static bool  g_dirty = true;      /* re-read the directory this frame */
static bool  g_initialized = false;
static float g_scroll = 0;
static char  g_status[96] = "";
static char  g_history[16][512];
static int   g_history_n;

/* Build "<cwd>/<name>" without doubling the root slash. */
static void join(char *out, size_t cap, const char *name) {
    if (strcmp(g_cwd, "/") == 0) snprintf(out, cap, "/%s", name);
    else                         snprintf(out, cap, "%s/%s", g_cwd, name);
}

static int cmp_entry(const void *a, const void *b) {
    const struct entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return (int)y->is_dir - (int)x->is_dir;  /* folders first */
    return strcmp(x->name, y->name);
}

static void read_dir(void) {
    g_count = 0;
    DIR *d = opendir(g_cwd);
    if (!d) { snprintf(g_status, sizeof g_status, "cannot open"); return; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_count < MAX_ENTRIES) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        struct entry *e = &g_entries[g_count];
        snprintf(e->name, sizeof e->name, "%s", de->d_name);

        char full[600];
        join(full, sizeof full, de->d_name);
        struct stat st;
        if (stat(full, &st) == 0) { e->is_dir = S_ISDIR(st.st_mode); e->size = (long)st.st_size; }
        else                      { e->is_dir = false; e->size = 0; }
        g_count++;
    }
    closedir(d);
    qsort(g_entries, g_count, sizeof g_entries[0], cmp_entry);
    snprintf(g_status, sizeof g_status, "%d items", g_count);

    char line[128];
    snprintf(line, sizeof line, "Files: %s -> %d items\n", g_cwd, g_count);
    embk_puts(1, line);
}

static void navigate_to(const char *path) {
    if (!path || path[0] != '/' || strcmp(path, g_cwd) == 0) return;
    if (g_history_n == 16) {
        memmove(g_history, g_history + 1, sizeof g_history - sizeof g_history[0]);
        g_history_n--;
    }
    snprintf(g_history[g_history_n++], sizeof g_history[0], "%s", g_cwd);
    snprintf(g_cwd, sizeof g_cwd, "%s", path);
    g_dirty = true; g_scroll = 0;
}

static void enter_dir(const char *name) {
    char nc[512];
    join(nc, sizeof nc, name);
    navigate_to(nc);
}

static void go_back(void) {
    if (g_history_n <= 0) return;
    snprintf(g_cwd, sizeof g_cwd, "%s", g_history[--g_history_n]);
    g_dirty = true; g_scroll = 0;
}

static void go_up(void) {
    if (strcmp(g_cwd, "/") == 0) return;
    char parent[512];
    snprintf(parent, sizeof parent, "%s", g_cwd);
    char *slash = strrchr(parent, '/');
    if (slash == parent) parent[1] = 0;
    else                 *slash = 0;
    navigate_to(parent);
}

static void sidebar_image_item(const char *image, const char *label, const char *path) {
    HStack(.spacing = 0, .width = 180, .align = Center) {
        if (ImageButtonKey(image, 28, path))
            navigate_to(path);
        if (Button(label).ghost().grow().leading().padding(-1).clicked())
            navigate_to(path);
    }
}

static void sidebar_item(int icon, const char *label, const char *path) {
    HStack(.spacing = 10, .width = 180, .align = Center) {
        Icon(icon).secondary();
        if (Button(label).ghost().grow().leading().clicked())
            navigate_to(path);
    }
}

static void sidebar_placeholder(int icon, const char *label) {
    HStack(.spacing = 10, .width = 180, .align = Center) {
        Icon(icon).tertiary();
        if (Button(label).ghost().grow().leading().clicked())
            snprintf(g_status, sizeof g_status, "%s is not mounted yet", label);
    }
}

static void open_file(const char *name);

static void folder_cell(int index) {
    struct entry *e = &g_entries[index];
    VStack(.spacing = 5, .width = 126, .align = Center) {
        if (e->is_dir) {
            /* Every folder uses the same bitmap, but must retain an independent
             * interaction identity or their hover states become shared. */
            if (ImageButtonKey("/system/images/pam/icon-files.pam", 62, e))
                enter_dir(e->name);
        } else {
            if (IconButton(IconDoc).frame(62, 62).font(Title).clicked())
                open_file(e->name);
        }
        if (Button(e->name).ghost().width(122).clicked()) {
            if (e->is_dir) enter_dir(e->name);
            else           open_file(e->name);
        }
    }
}

static void open_file(const char *name) {
    char full[600], envbuf[640];
    join(full, sizeof full, name);
    snprintf(envbuf, sizeof envbuf, "EDIT_FILE=%s", full);

    char *argv[] = { "edit", NULL };
    char *env[]  = { envbuf, NULL };
    int64_t h = embk_spawn_env("/data/apps/edit/edit.elf", argv, env, NULL, 0);
    if (h >= 0) snprintf(g_status, sizeof g_status, "opened %s", name);
    else        snprintf(g_status, sizeof g_status, "open failed");
}

static void app(void) {
    if (!g_initialized) {
        /* Folder shortcuts launch this same app with FILES_PATH. A normal
         * launch starts in the signed-in user's HOME instead of exposing the
         * filesystem root as the first screen. */
        const char *start = getenv("FILES_PATH");
        if (!start || start[0] != '/') start = getenv("HOME");
        if (start && start[0] == '/')
            snprintf(g_cwd, sizeof g_cwd, "%s", start);
        g_initialized = true;
    }
    if (g_dirty) { read_dir(); g_dirty = false; }
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') home = "/";
    char desktop[560], documents[560], downloads[560], music[560];
    char pictures[560], videos[560], trash[560];
    snprintf(desktop,   sizeof desktop,   "%s/Desktop",   home);
    snprintf(documents, sizeof documents, "%s/Documents", home);
    snprintf(downloads, sizeof downloads, "%s/Downloads", home);
    snprintf(music,     sizeof music,     "%s/Music",     home);
    snprintf(pictures,  sizeof pictures,  "%s/Pictures",  home);
    snprintf(videos,    sizeof videos,    "%s/Videos",    home);
    snprintf(trash,     sizeof trash,     "%s/Trash",     home);

    Window("Files") {
        HStack(.spacing = 0, .height = em_viewport_height(), .align = Fill) {
            VStack(.spacing = 8, .padding = 14, .width = 210, .align = Fill,
                   .background = { .r=.10f, .g=.105f, .b=.12f, .a=1.0f }) {
                HStack(.align = Center) {
                    Icon(IconSearch).secondary();
                    Text("Files").heading();
                }
                Divider();
                sidebar_image_item("/system/images/pam/icon-home.pam",      "Home",      home);
                sidebar_image_item("/system/images/pam/icon-star.pam",      "Desktop",   desktop);
                sidebar_image_item("/system/images/pam/icon-documents.pam", "Documents", documents);
                sidebar_image_item("/system/images/pam/icon-downloads.pam", "Downloads", downloads);
                sidebar_image_item("/system/images/pam/icon-music.pam",     "Music",     music);
                sidebar_image_item("/system/images/pam/icon-images.pam",    "Pictures",  pictures);
                sidebar_image_item("/system/images/pam/icon-videos.pam",    "Videos",    videos);
                sidebar_image_item("/system/images/pam/icon-trash.pam",     "Trash",     trash);
                Divider();
                Text("Disks").caption().tertiary();
                sidebar_item(IconFiles, "System Disk", "/");
                sidebar_placeholder(IconFiles, "External Disk");
                Divider();
                sidebar_item(IconFiles, "Other Locations", "/");
                Spacer();
                Text("EmbLink OS").caption().tertiary();
            }

            VStack(.spacing = 12, .padding = 14, .grow = 1, .align = Fill) {
                HStack(.spacing = 8, .align = Center) {
                    if (IconButton(IconChevronL).clicked()) go_back();
                    if (IconButton(IconChevronU).clicked()) go_up();
                    Card(.padding = 9, .grow = 1, .corner = 8,
                         .background = { .r=.14f, .g=.145f, .b=.16f, .a=1.0f }) {
                        Text(g_cwd).body();
                    }
                    Text(g_status).caption().secondary();
                }

                Divider();

                ScrollView(&g_scroll, 500) {
                    VStack(.spacing = 16, .align = Fill) {
                        int cols = ((int)em_viewport_width() - 260) / 142;
                        if (cols < 2) cols = 2;
                        if (cols > 7) cols = 7;
                        for (int row = 0; row * cols < g_count; row++) {
                            HStack(.spacing = 14, .align = Leading) {
                                for (int col = 0; col < cols; col++) {
                                    int i = row * cols + col;
                                    if (i < g_count) folder_cell(i);
                                    else             Spacer();
                                }
                            }
                        }
                        if (g_count == 0)
                            EmptyState(IconFolder, "Empty folder",
                                       "This location does not contain any files yet.");
                    }
                }
            }
        }
    }
}

EM_APPLICATION {
    .title  = "Files",
    .size   = { 1100, 700 },
    .theme  = Dark,
    .resize = Resizable,
    .view   = app,
};
