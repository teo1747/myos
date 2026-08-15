#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "embk.h"
#include "em.h"

#define SESSION_FD 3

static char username[EMBK_AUTH_USERNAME_MAX + 1];
static char password[EMBK_AUTH_PASSWORD_MAX + 1];
static char status[96] = "Enter your account details.";

static void sign_in(void) {
    if (!embk_auth_verify(username, password)) {
        memset(password, 0, sizeof password);
        snprintf(status, sizeof status, "Incorrect username or password.");
        return;
    }
    size_t len = strlen(username);
    if (embk_write(SESSION_FD, username, len) != (int64_t)len) {
        snprintf(status, sizeof status, "Could not start the session.");
        return;
    }
    memset(password, 0, sizeof password);
    em_app_request_exit(0);
}

static void view(void) {
    float panel = em_viewport_width() * 0.34f;
    if (panel < 360) panel = 360;
    if (panel > 480) panel = 480;
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
                Text("EmbLink OS").title();
                Text("Sign in").heading();
                Text(status).body().secondary();
                TextField(username, sizeof username, "username");
                PasswordField(password, sizeof password, "password");
                if (Button("Open session").primary().clicked()) sign_in();
            }
            Spacer();
        }
    }
}

EM_APPLICATION {
    .title = "EmbLink OS Login",
    .size = { 680, 460 },
    .theme = Dark,
    .chrome = Chromeless,
    .resize = FixedSize,
    .fullscreen = 1,
    .view = view,
};
