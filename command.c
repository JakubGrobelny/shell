#include "shell.h"

typedef int (*func_t)(char** argv);

typedef struct {
    const char* name;
    func_t func;
} command_t;

static int do_quit(char** argv) {
    exit(EXIT_SUCCESS);
}

/*
 * Change current working directory.
 * 'cd' - change to $HOME
 * 'cd path' - change to provided path
 */
static int do_chdir(char** argv) {
    char* path = argv[0];
    if (path == NULL)
        path = getenv("HOME");
    int rc = chdir(path);
    if (rc < 0) {
        msg("cd: %s: %s\n", strerror(errno), path);
        return 1;
    }
    return 0;
}

/*
 * Displays all stopped or running jobs.
 */
static int do_jobs(char** argv) {
    watchjobs(ALL);
    return 0;
}

/*
 * Move running or stopped background job to foreground.
 * 'fg' choose highest numbered job
 * 'fg n' choose job number n
 */
static int do_fg(char** argv) {
    int j = argv[0] ? atoi(argv[0]) : -1;

    sigset_t mask;
    Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);
    if (!resumejob(j, FG, &mask))
        msg("fg: job not found: %s\n", argv[0]);
    Sigprocmask(SIG_SETMASK, &mask, NULL);
    return 0;
}

/*
 * Make stopped background job running.
 * 'bg' choose highest numbered job
 * 'bg n' choose job number n
 */
static int do_bg(char** argv) {
    int j = argv[0] ? atoi(argv[0]) : -1;

    sigset_t mask;
    Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);
    if (!resumejob(j, BG, &mask))
        msg("bg: job not found: %s\n", argv[0]);
    Sigprocmask(SIG_SETMASK, &mask, NULL);
    return 0;
}

/*
 * Make stopped background job running.
 * 'bg' choose highest numbered job
 * 'bg n' choose job number n
 */
static int do_kill(char** argv) {
    if (!argv[0])
        return -1;
    if (*argv[0] != '%')
        return -1;

    int j = atoi(argv[0] + 1);

    sigset_t mask;
    Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);
    if (!killjob(j))
        msg("kill: job not found: %s\n", argv[0]);
    Sigprocmask(SIG_SETMASK, &mask, NULL);

    return 0;
}

static command_t builtins[] = {
    {"quit", do_quit}, {"cd", do_chdir},    {"jobs", do_jobs}, {"fg", do_fg},
    {"bg", do_bg},         {"kill", do_kill}, {NULL, NULL},
};

int builtin_command(char** argv) {
    for (command_t* cmd = builtins; cmd->name; cmd++) {
        if (strcmp(argv[0], cmd->name))
            continue;
        return cmd->func(&argv[1]);
    }

    errno = ENOENT;
    return -1;
}

noreturn void external_command(char** argv) {
    const char* path = getenv("PATH");

    if (!index(argv[0], '/') && path) {
        /* DONE: For all paths in PATH construct an absolute path and execve it. */
        do {
            size_t len = strcspn(path, ":");
            if (!len) {
                break;
            }
            char* abs_path = strndup(path, len);
            strapp(&abs_path, "/");
            strapp(&abs_path, argv[0]);
            execve(abs_path, argv, environ);
            free(abs_path);
            path += len;
        } while (*(path++));

    } else {
        (void)execve(argv[0], argv, environ);
    }

    msg("%s: %s\n", argv[0], strerror(errno));
    exit(EXIT_FAILURE);
}
