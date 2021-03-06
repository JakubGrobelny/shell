#include <stdio.h> // to remove compilation errors from readline.h on Arch
#include <readline/readline.h>
#include <readline/history.h>

#define DEBUG 0
#include "shell.h"

sigset_t sigchld_mask;

static sigjmp_buf loop_env;

static void sigint_handler(int sig) {
    siglongjmp(loop_env, sig);
}

/* Consume all tokens related to redirection operators.
 * Put opened file descriptors into inputp & output respectively. */
static int do_redir(token_t* token, int ntokens, int* inputp, int* outputp) {
    token_t mode = NULL; /* T_INPUT, T_OUTPUT or NULL */
    int n = 0;           /* number of tokens after redirections are removed */

    for (int i = 0; i < ntokens && token[i] != T_NULL; i++) {
    /* DONE: Handle tokens and open files as requested. */
        if (token[i] == T_INPUT || token[i] == T_OUTPUT) {
            assert(i+1 != ntokens && string_p(token[i+1]));

            mode = token[i];
            char* filename = token[i+1];
    
            int flags = mode == T_INPUT
                      ? O_RDONLY
                      : O_WRONLY | O_CREAT;
    
            int* fd = mode == T_INPUT
                    ? inputp
                    : outputp;

            mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

            if (*fd != -1) {
                close(*fd);
            }

            *fd = open(filename, flags, mode);

            for (int j = i+2; j < ntokens; j++) {
                token[j-2] = token[j];
            }

            token[ntokens-2] = T_NULL;
            token[ntokens-1] = T_NULL;
            i--;

        } else {
            n++;
        }
    }

    token[n] = NULL;
    return n;
}

/* Execute internal command within shell's process or execute external command
 * in a subprocess. External command can be run in the background. */
static int do_job(token_t* token, int ntokens, bool bg) {
    int input = -1, output = -1;
    int exitcode = 0;

    ntokens = do_redir(token, ntokens, &input, &output);

    if ((exitcode = builtin_command(token)) >= 0)
        return exitcode;

    sigset_t mask;
    Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

    /* DONE:: Start a subprocess, create a job and monitor it. */    
    pid_t pid = Fork();

    if (!pid) {
        sigset_t clear_mask;
        sigemptyset(&clear_mask);
        Sigprocmask(SIG_SETMASK, &clear_mask, NULL);
        Signal(SIGTSTP, SIG_DFL);
        Signal(SIGTTIN, SIG_DFL);
        Signal(SIGTTOU, SIG_DFL);

        if (output != -1) {
            dup2(output, STDOUT_FILENO);
        }

        if (input != -1) {
            dup2(input, STDIN_FILENO);
        }
        
        external_command(token);
    }

    setpgid(pid, pid);
    int job = addjob(pid, bg);
    addproc(job, pid, token);

    if (output != -1) {
        close(output);
    }

    if (input != -1) {
        close(input);
    }

    if (!bg) {
        exitcode = monitorjob(&mask);
    } else {
        msg("[%d] running '%s'\n", job, jobcmd(job));
    }

    Sigprocmask(SIG_SETMASK, &mask, NULL);
    return exitcode;
}

/* Start internal or external command in a subprocess that belongs to pipeline.
 * All subprocesses in pipeline must belong to the same process group. */
static pid_t do_stage(
    pid_t pgid, 
    sigset_t* mask, 
    int input, 
    int output,
    token_t* token, 
    int ntokens
) {
    ntokens = do_redir(token, ntokens, &input, &output);

    /* DONE: Start a subprocess and make sure it's moved to a process group. */
    pid_t pid = Fork();

    if (!pid) {
        Sigprocmask(SIG_SETMASK, mask, NULL);
        Signal(SIGTSTP, SIG_DFL);
        Signal(SIGTTIN, SIG_DFL);
        Signal(SIGTTOU, SIG_DFL);

        if (input != -1) {
            dup2(input, STDIN_FILENO);
        }

        if (output != -1) {
            dup2(output, STDOUT_FILENO);
        }

        int exitcode;
        if ((exitcode = builtin_command(token)) >= 0) {
            exit(exitcode);
        }

        external_command(token);
    }
    
    setpgid(pid, pgid);
    return pid;
}

static void mkpipe(int* readp, int* writep) {
    int fds[2];
    Pipe(fds);
    *readp = fds[0];
    *writep = fds[1];
}

/* Pipeline execution creates a multiprocess job. Both internal and external
 * commands are executed in subprocesses. */
static int do_pipeline(token_t* token, int ntokens, bool bg) {
    pid_t pid, pgid = 0;
    int job = -1;
    int exitcode = 0;

    int input = -1, output = -1, next_input = -1;

    mkpipe(&next_input, &output);

    sigset_t mask;
    Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

    /* DONE: Start pipeline subprocesses, create a job and monitor it.
     * Remember to close unused pipe ends! */
    size_t stage_start = 0;
    while (stage_start < ntokens) {
        size_t stage_end;
        bool is_last = true;
        for (stage_end = stage_start; stage_end < ntokens; stage_end++) {
            if (token[stage_end] == T_PIPE) {
                token[stage_end] = T_NULL;
                is_last = false;
                break;
            }
        }

        token_t* stage_tokens = token + stage_start;
        size_t tok_len = stage_end - stage_start;

        if (is_last) {
            output = -1;
        }

        pid = do_stage(pgid, &mask, input, output, stage_tokens, tok_len);

        if (output != -1) {
            close(output);
        }

        if (input != -1) {
            close(input);
        }

        if (job == -1) {
            pgid = pid;
            job = addjob(pgid, bg);
        }

        addproc(job, pid, stage_tokens);

        input = next_input;
        if (!is_last) {
            mkpipe(&next_input, &output);
        }

        stage_start = stage_end + 1;
    }

    close(next_input);

    if (!bg) {
        monitorjob(&mask);
    } else {
        msg("[%d] running '%s'\n", job, jobcmd(job));
    }

    Sigprocmask(SIG_SETMASK, &mask, NULL);
    return exitcode;
}

static bool is_pipeline(token_t* token, int ntokens) {
    for (int i = 0; i < ntokens; i++)
        if (token[i] == T_PIPE)
            return true;
    return false;
}

static void eval(char* cmdline) {
    bool bg = false;
    int ntokens;
    token_t* token = tokenize(cmdline, &ntokens);

    if (ntokens > 0 && token[ntokens - 1] == T_BGJOB) {
        token[--ntokens] = NULL;
        bg = true;
    }

    if (ntokens > 0) {
        if (is_pipeline(token, ntokens)) {
            do_pipeline(token, ntokens, bg);
        } else {
            do_job(token, ntokens, bg);
        }
    }

    free(token);
}

int main(int argc, char* argv[]) {
    rl_initialize();

    sigemptyset(&sigchld_mask);
    sigaddset(&sigchld_mask, SIGCHLD);

    initjobs();

    Signal(SIGINT, sigint_handler);
    Signal(SIGTSTP, SIG_IGN);
    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    char* line;
    while (true) {
        if (!sigsetjmp(loop_env, 1)) {
            line = readline("# ");
        } else {
            msg("\n");
            continue;
        }

        if (line == NULL)
            break;

        if (strlen(line)) {
            add_history(line);
            eval(line);
        }
        free(line);
        watchjobs(FINISHED);
    }

    msg("\n");
    shutdownjobs();

    return 0;
}
