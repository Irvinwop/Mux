#define _GNU_SOURCE

#include <errno.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    gboolean new_layer = argc > 1 && g_strcmp0(argv[1], "--new") == 0;
    int view_argument = new_layer ? 2 : 1;

    gchar *generated = NULL;
    const gchar *layer = g_getenv("MUX_LAYER");
    if (new_layer || !layer || !*layer) {
        gchar *uuid = g_uuid_string_random();
        generated = g_strdup_printf("layer-%.8s", uuid);
        g_free(uuid);
        layer = generated;
    }

    g_setenv("MUX_LAYER", layer, TRUE);
    gchar *layer_assignment = g_strdup_printf("MUX_LAYER=%s", layer);
    gchar *bar_arguments[] = {
        "kitten",
        "@",
        "launch",
        "--location=hsplit",
        "--bias=8",
        "--keep-focus",
        "--copy-env",
        "--env",
        layer_assignment,
        "--env",
        "MUX_GLOBAL_BAR=1",
        "--title",
        "Mux Bar",
        "mux-bar",
        NULL,
    };

    gint status = 0;
    GError *error = NULL;
    gboolean launched = g_spawn_sync(
        NULL,
        bar_arguments,
        NULL,
        G_SPAWN_SEARCH_PATH,
        NULL,
        NULL,
        NULL,
        NULL,
        &status,
        &error);

    if (launched && g_spawn_check_wait_status(status, &error))
        g_setenv("MUX_GLOBAL_BAR", "1", TRUE);
    else {
        g_printerr(
            "mux-layer: could not launch global bar: %s\n",
            error ? error->message : g_strerror(errno));
        g_unsetenv("MUX_GLOBAL_BAR");
    }
    g_clear_error(&error);
    g_free(layer_assignment);
    g_free(generated);

    int remaining = argc - view_argument;
    gchar **view_arguments = g_new0(gchar *, (gsize)remaining + 2);
    view_arguments[0] = "mux-pane";
    for (int i = 0; i < remaining; i++)
        view_arguments[i + 1] = argv[view_argument + i];

    execvp("mux-pane", view_arguments);
    g_printerr("mux-layer: exec mux-pane failed: %s\n", g_strerror(errno));
    g_free(view_arguments);
    return EXIT_FAILURE;
}
