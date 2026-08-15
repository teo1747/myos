#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "embk.h"
#include "em.h"

static char username[EMBK_AUTH_USERNAME_MAX + 1];
static char password[EMBK_AUTH_PASSWORD_MAX + 1];
static char confirm[EMBK_AUTH_PASSWORD_MAX + 1];
static char status[128] = "Choose the account used to open your desktop.";

static int write_all(const char *path, const char *data) {
    int fd = (int)embk_open(path, EMBK_O_CREAT | EMBK_O_TRUNC | EMBK_O_WRONLY, 0600);
    if (fd < 0) return -1;
    size_t len = strlen(data), off = 0;
    while (off < len) {
        int64_t n = embk_write(fd, data + off, len - off);
        if (n <= 0) { embk_close(fd); return -1; }
        off += (size_t)n;
    }
    embk_close(fd);
    return 0;
}

static void create_home_folders(const char *home) {
    static const char *names[] = {
        "Desktop", "Documents", "Downloads", "Music",
        "Pictures", "Videos", "Trash"
    };
    char path[128];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", home, names[i]);
        (void)embk_mkdir(path);
    }
}

static void create_account(void) {
    if (!embk_auth_valid_username(username)) {
        snprintf(status, sizeof status, "Use lowercase letters, numbers, - or _.");
        return;
    }
    if (strlen(password) < 8) {
        snprintf(status, sizeof status, "The password needs at least 8 characters.");
        return;
    }
    if (strcmp(password, confirm) != 0) {
        snprintf(status, sizeof status, "The two passwords do not match.");
        return;
    }

    char home[96], profile[112], manifest[320];
    snprintf(home, sizeof home, "/home/%s", username);
    snprintf(profile, sizeof profile, "%s/user.ns", home);
    struct embk_stat st;
    if (embk_mkdir(home) < 0 &&
        (embk_stat(home, &st) < 0 || st.type != EMBK_DT_DIR)) {
        snprintf(status, sizeof status, "Could not create the user directory.");
        return;
    }
    create_home_folders(home);
    snprintf(manifest, sizeof manifest,
             "# Session profile for %s\nro /system\nro /data/apps\nrw %s\nrw /run\n",
             username, home);
    if (write_all(profile, manifest) < 0) {
        snprintf(status, sizeof status, "Could not create the session profile.");
        return;
    }
    int rc = embk_auth_create(username, password);
    memset(password, 0, sizeof password);
    memset(confirm, 0, sizeof confirm);
    if (rc != 0) {
        snprintf(status, sizeof status, "Account creation failed (%d).", rc);
        return;
    }
    em_app_request_exit(0);
}

static void view(void) {
    float panel = em_viewport_width() * 0.38f;
    if (panel < 380) panel = 380;
    if (panel > 520) panel = 520;
    float side = em_viewport_width() * 0.07f;
    if (side < 32) side = 32;
    if (side > 120) side = 120;
    Screen(.padding = -1, .justify = Center, .align = Center) {
        BackgroundImage("/system/images/ppm/colibri-user.ppm");
        HStack(.width = em_viewport_width(), .height = em_viewport_height(),
               .padding = side, .align = Center) {
            Glass(.width = panel, .spacing = 14, .padding = 32, .align = Fill,
                  .background = { .r = 0.045f, .g = 0.055f, .b = 0.09f, .a = 0.92f },
                  .corner = 20, .border = 1, .shadow = 1) {
                Text("Welcome to EmbLink OS").title();
                Text("Create your first account").heading();
                Text(status).body().secondary();
                TextField(username, sizeof username, "username");
                PasswordField(password, sizeof password, "password");
                PasswordField(confirm, sizeof confirm, "confirm password");
                if (Button("Create account").primary().clicked()) create_account();
            }
            Spacer();
        }
    }
}

EM_APPLICATION {
    .title = "EmbLink OS Setup",
    .size = { 720, 520 },
    .theme = Dark,
    .chrome = Chromeless,
    .resize = FixedSize,
    .fullscreen = 1,
    .view = view,
};
