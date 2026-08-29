#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t supervised_pane_pid;

static void
forward_stop(int signal_number)
{
    pid_t pid = (pid_t)supervised_pane_pid;

    if (pid > 0)
        (void)kill(pid, signal_number);
}

static int
wait_status_exit_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return EXIT_FAILURE;
}

static gboolean
run_kitty(gchar **arguments, GError **error)
{
    g_autofree gchar *standard_output = NULL;
    g_autofree gchar *standard_error = NULL;
    gint status = 0;

    if (!g_spawn_sync(NULL,
                      arguments,
                      NULL,
                      G_SPAWN_SEARCH_PATH,
                      NULL,
                      NULL,
                      &standard_output,
                      &standard_error,
                      &status,
                      error) ||
        !g_spawn_check_wait_status(status, error)) {
        if (standard_error && *standard_error)
            g_prefix_error(error, "%s: ", g_strstrip(standard_error));
        return FALSE;
    }
    return TRUE;
}

static int
launch_split(const gchar *location, int argc, char **argv)
{
    const gchar *window = g_getenv("KITTY_WINDOW_ID");
    g_autofree gchar *target = NULL;
    g_autoptr(GPtrArray) arguments = NULL;
    g_autoptr(GError) error = NULL;

    if (!window || !*window) {
        g_printerr("mux-layer: split launch has no Kitty window identity\n");
        return EXIT_FAILURE;
    }
    target = g_strdup_printf("id:%s", window);
    arguments = g_ptr_array_new();
    g_ptr_array_add(arguments, "kitten");
    g_ptr_array_add(arguments, "@");
    g_ptr_array_add(arguments, "launch");
    g_ptr_array_add(arguments, "--next-to");
    g_ptr_array_add(arguments, target);
    g_ptr_array_add(arguments, "--location");
    g_ptr_array_add(arguments, (gpointer)location);
    g_ptr_array_add(arguments, "--copy-env");
    g_ptr_array_add(arguments, "--title");
    g_ptr_array_add(arguments, "MUX loading");
    g_ptr_array_add(arguments, "mux-pane");
    for (int i = 2; i < argc; i++)
        g_ptr_array_add(arguments, argv[i]);
    g_ptr_array_add(arguments, NULL);

    if (!run_kitty((gchar **)arguments->pdata, &error)) {
        g_printerr("mux-layer: Kitty split launch failed: %s\n",
                   error ? error->message : g_strerror(errno));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static pid_t
start_content(int argc, char **argv, int first_argument, GError **error)
{
    int error_pipe[2] = { -1, -1 };
    pid_t child;
    int child_errno = 0;
    ssize_t count;
    int remaining = argc - first_argument;
    g_auto(GStrv) arguments = g_new0(gchar *, (gsize)remaining + 2);

    arguments[0] = g_strdup("mux-pane");
    for (int i = 0; i < remaining; i++)
        arguments[i + 1] = g_strdup(argv[first_argument + i]);
    if (pipe2(error_pipe, O_CLOEXEC) < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "create pane exec pipe: %s",
                    g_strerror(errno));
        return -1;
    }
    child = fork();
    if (child < 0) {
        int saved_errno = errno;

        close(error_pipe[0]);
        close(error_pipe[1]);
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    "fork mux-pane: %s",
                    g_strerror(saved_errno));
        return -1;
    }
    if (child == 0) {
        close(error_pipe[0]);
        execvp(arguments[0], arguments);
        child_errno = errno;
        (void)write(error_pipe[1], &child_errno, sizeof(child_errno));
        _exit(127);
    }

    close(error_pipe[1]);
    do {
        count = read(error_pipe[0], &child_errno, sizeof(child_errno));
    } while (count < 0 && errno == EINTR);
    close(error_pipe[0]);
    if (count > 0) {
        int status;

        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
            ;
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(child_errno),
                    "exec mux-pane: %s",
                    g_strerror(child_errno));
        return -1;
    }
    if (count < 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    "monitor mux-pane exec: %s",
                    g_strerror(errno));
        (void)kill(child, SIGTERM);
        return -1;
    }
    return child;
}

static gboolean
launch_bar(const gchar *layer, GError **error)
{
    const gchar *window = g_getenv("KITTY_WINDOW_ID");
    g_autofree gchar *target = NULL;
    g_autofree gchar *layer_assignment = NULL;

    if (!window || !*window) {
        g_set_error_literal(error,
                            G_FILE_ERROR,
                            G_FILE_ERROR_FAILED,
                            "content pane has no Kitty window identity");
        return FALSE;
    }
    target = g_strdup_printf("id:%s", window);
    layer_assignment = g_strdup_printf("MUX_LAYER=%s", layer);
    gchar *arguments[] = {
        "kitten", "@", "launch",
        "--next-to", target,
        "--location=hsplit", "--bias=8", "--keep-focus", "--copy-env",
        "--env", layer_assignment,
        "--env", "MUX_GLOBAL_BAR=1",
        "--title", "MUX-BAR",
        "mux-bar", NULL,
    };

    return run_kitty(arguments, error);
}

int
main(int argc, char **argv)
{
    gboolean new_layer = argc > 1 && g_strcmp0(argv[1], "--new") == 0;
    int view_argument = new_layer ? 2 : 1;
    g_autofree gchar *generated = NULL;
    const gchar *layer = g_getenv("MUX_LAYER");
    g_autoptr(GError) error = NULL;
    pid_t child;
    int status;

    if (argc > 1 && g_strcmp0(argv[1], "--split=vsplit") == 0)
        return launch_split("vsplit", argc, argv);
    if (argc > 1 && g_strcmp0(argv[1], "--split=hsplit") == 0)
        return launch_split("hsplit", argc, argv);

    if (new_layer || !layer || !*layer) {
        g_autofree gchar *uuid = g_uuid_string_random();

        generated = g_strdup_printf("layer-%.8s", uuid);
        layer = generated;
    }
    g_setenv("MUX_LAYER", layer, TRUE);

    child = start_content(argc, argv, view_argument, &error);
    if (child < 0) {
        g_printerr("mux-layer: content startup failed: %s\n", error->message);
        return EXIT_FAILURE;
    }
    supervised_pane_pid = (sig_atomic_t)child;
    signal(SIGINT, forward_stop);
    signal(SIGTERM, forward_stop);
    signal(SIGHUP, forward_stop);

    if (waitpid(child, &status, WNOHANG) == child) {
        supervised_pane_pid = 0;
        return wait_status_exit_code(status);
    }
    if (!launch_bar(layer, &error))
        g_printerr("mux-layer: global bar launch failed: %s\n", error->message);

    do {
        child = waitpid((pid_t)supervised_pane_pid, &status, 0);
    } while (child < 0 && errno == EINTR);
    supervised_pane_pid = 0;
    return child < 0 ? EXIT_FAILURE : wait_status_exit_code(status);
}
