#include "smallsh.h"
 
/* program buffers and work pointers */
static char inpbuf[MAXBUF], tokbuf[2 * MAXBUF],
            *ptr = inpbuf, *tok = tokbuf;
 
char *prompt = "Command> ";   /* prompt */
 
/* NEW: job-control state */
static pid_t shell_pgid;      /* process group the shell runs in        */
static int   interactive;     /* 1 if stdin is a terminal, 0 otherwise  */
 
/* ------------------------------------------------------------------ */
/* NEW: signal handling                                                */
/* ------------------------------------------------------------------ */
 
/* NEW: the shell's own SIGINT handler.
 * Ctrl+C at the prompt should NOT kill the shell; it just gives a fresh
 * prompt, like bash. Only async-signal-safe calls (write, strlen) are
 * allowed inside a handler, so no printf here. */
static void sigint_handler(int sig)
{
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);
    write(STDOUT_FILENO, prompt, strlen(prompt));
}
 
/* NEW: small helper around sigaction() */
static void set_signal(int sig, void (*handler)(int), int flags)
{
    struct sigaction sa;
 
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = flags;
    if (sigaction(sig, &sa, NULL) == -1)
        perror("smallsh: sigaction");
}
 
/* NEW: called once at startup */
void init_shell(void)
{
    interactive = isatty(STDIN_FILENO);
 
    /* SA_RESTART: if Ctrl+C arrives while getchar() or waitpid() is
     * blocked, restart the call instead of failing with EINTR.
     * Without it, getchar() would return EOF and the shell would quit. */
    set_signal(SIGINT, sigint_handler, SA_RESTART);
 
    set_signal(SIGQUIT, SIG_IGN, 0);   /* Ctrl+\ : don't core-dump shell   */
    set_signal(SIGTSTP, SIG_IGN, 0);   /* Ctrl+Z : don't stop the shell    */
    set_signal(SIGTTIN, SIG_IGN, 0);   /* needed to take the terminal back */
    set_signal(SIGTTOU, SIG_IGN, 0);   /*   with tcsetpgrp()               */
 
    /* Put the shell in its own process group and make that group the
     * terminal's foreground group. (setpgid fails with EPERM if we are
     * already a session leader; that's fine, we're already a group leader.) */
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) == -1 && errno != EPERM)
        perror("smallsh: setpgid");
    shell_pgid = getpgrp();
 
    if (interactive)
        tcsetpgrp(STDIN_FILENO, shell_pgid);
}
 
/* NEW: in the child, undo the shell's signal settings before exec.
 * Ignored signals stay ignored across exec, so this step is required,
 * otherwise commands could not be interrupted with Ctrl+C or Ctrl+Z. */
static void child_reset_signals(void)
{
    set_signal(SIGINT,  SIG_DFL, 0);
    set_signal(SIGQUIT, SIG_DFL, 0);
    set_signal(SIGTSTP, SIG_DFL, 0);
    set_signal(SIGTTIN, SIG_DFL, 0);
    set_signal(SIGTTOU, SIG_DFL, 0);
    set_signal(SIGCHLD, SIG_DFL, 0);
}
 
/* ------------------------------------------------------------------ */
/* NEW: foreground / background process management                     */
/* ------------------------------------------------------------------ */
 
/* NEW: wait for one foreground child.
 * WUNTRACED makes waitpid also return if the child is STOPPED (Ctrl+Z),
 * otherwise the shell would hang forever waiting on a stopped process. */
static int wait_foreground(pid_t pid)
{
    int status;
    pid_t r;
 
    do {
        r = waitpid(pid, &status, WUNTRACED);
    } while (r == -1 && errno == EINTR);
 
    if (r == -1) {
        perror("smallsh: waitpid");
        return (-1);
    }
 
    if (WIFSTOPPED(status)) {
        printf("\n[Process id %d] Stopped\n", pid);
        /* TEAMMATE HOOK: add pid to the job table as "Stopped" here so
         * `jobs`, `fg`, and `bg` can find it. */
    } else if (WIFSIGNALED(status)) {
        if (WTERMSIG(status) == SIGINT)
            printf("\n");   /* Ctrl+C: just move to a clean line */
        else
            printf("[Process id %d] Terminated by signal %d\n",
                   pid, WTERMSIG(status));
    }
 
    return status;
}
 
/* NEW: collect any background children that have finished.
 * WNOHANG = don't block if nobody has finished yet.
 * Called before each prompt, when no foreground job is running, so any
 * child reaped here must be a background job. This prevents zombies. */
void reap_background(void)
{
    pid_t pid;
    int status;
 
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status))
            printf("[Process id %d] Done (exit status %d)\n",
                   pid, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            printf("[Process id %d] Killed by signal %d\n",
                   pid, WTERMSIG(status));
        /* TEAMMATE HOOK: remove pid from the job table here. */
    }
}
 
/* ------------------------------------------------------------------ */
/* Original textbook routines                                          */
/* ------------------------------------------------------------------ */
 
/* print prompt and read a line */
int userin(char *p)
{
    int c, count;
 
    /* initialization for later routines */
    ptr = inpbuf;
    tok = tokbuf;
 
    /* display prompt */
    printf("%s", p);
    fflush(stdout);                     /* NEW: make sure prompt shows */
 
    count = 0;
 
    while (1)
    {
        if ((c = getchar()) == EOF)
            return (EOF);
 
        if (count < MAXBUF)
            inpbuf[count++] = c;
 
        if (c == '\n' && count < MAXBUF)
        {
            inpbuf[count] = '\0';
            return count;
        }
 
        /* if line too long restart */
        if (c == '\n')
        {
            printf("smallsh: input line too long\n");
            count = 0;
            printf("%s ", p);
        }
    }
}
 
/* get token, place into tokbuf */
int gettok(char **outptr)
{
    int type;
 
    /* set the outptr string to tok */
    *outptr = tok;
 
    /* strip white space from the buffer containing the tokens */
    while (*ptr == ' ' || *ptr == '\t')
        ptr++;
 
    /* set the token pointer to the first token in the buffer */
    *tok++ = *ptr;
 
    /* set the type variable depending on the token in the buffer */
    switch (*ptr++) {
    case '\n':
        type = EOL;
        break;
    case '&':
        type = AMPERSAND;
        break;
    case ';':
        type = SEMICOLON;
        break;
    default:
        type = ARG;
        /* keep reading valid ordinary characters */
        while (inarg(*ptr))
            *tok++ = *ptr++;
    }
 
    *tok++ = '\0';
    return type;
}
 
static char special[] = {' ', '\t', '&', ';', '\n', '\0'};
 
/* is c an ordinary (non-special) character? */
int inarg(char c)
{
    char *wrk;
 
    for (wrk = special; *wrk; wrk++)
    {
        if (c == *wrk)
            return (0);
    }
 
    return (1);
}
 
/* process input line */
int procline(void)
{
    char *arg[MAXARG + 1];   /* pointer array for runcommand */
    int toktype;             /* type of token in command */
    int narg;                /* number of arguments so far */
    int type;                /* FOREGROUND or BACKGROUND */
 
    narg = 0;
 
    for (;;)                 /* loop forever */
    {
        /* take action according to token type */
        switch (toktype = gettok(&arg[narg])) {
        case ARG:
            if (narg < MAXARG)
                narg++;
            break;
 
        case EOL:
        case SEMICOLON:
        case AMPERSAND:
            if (toktype == AMPERSAND)
                type = BACKGROUND;
            else
                type = FOREGROUND;
 
            if (narg != 0)
            {
                arg[narg] = NULL;
                /* TEAMMATE HOOK: check arg[0] for built-ins
                 * (jobs, fg, bg, kill) here before calling runcommand. */
                runcommand(arg, type);
            }
 
            if (toktype == EOL)
                return 0;
 
            narg = 0;
            break;
        }
    }
}
 
/* execute a command with optional wait */
int runcommand(char **cline, int where)
{
    pid_t pid;
    int status;
 
    fflush(stdout);          /* NEW: don't let the child inherit unflushed output */
 
    switch (pid = fork()) {
    case -1:
        perror("smallsh");
        return (-1);
    case 0:
        /* NEW: child goes into its own process group (pgid = its pid). */
        setpgid(0, 0);
        /* NEW: foreground child takes the terminal, so Ctrl+C/Ctrl+Z
         * go to it. Done BEFORE resetting signals, while SIGTTOU is
         * still ignored. */
        if (where == FOREGROUND && interactive)
            tcsetpgrp(STDIN_FILENO, getpid());
        child_reset_signals();   /* NEW */
 
        execvp(*cline, cline);
        perror(*cline);
        exit(1);
    }
 
    /* code for parent */
 
    /* NEW: parent sets the child's group too. Both sides do it because we
     * don't know whether parent or child runs first after fork (race). */
    setpgid(pid, pid);
 
    /* if background process print pid and exit */
    if (where == BACKGROUND)
    {
        printf("[Process id %d]\n", pid);
        /* TEAMMATE HOOK: add pid to the job table as "Running" here. */
        return (0);
    }
 
    /* NEW: foreground: hand the terminal to the child's process group */
    if (interactive)
        tcsetpgrp(STDIN_FILENO, pid);
 
    /* wait until process pid exits (or is stopped) */
    status = wait_foreground(pid);    /* NEW: replaces plain waitpid */
 
    /* NEW: take the terminal back for the shell */
    if (interactive)
        tcsetpgrp(STDIN_FILENO, shell_pgid);
 
    return status;
}
 
int main(void)
{
    init_shell();                       /* NEW */
 
    for (;;) {
        reap_background();              /* NEW: report finished bg jobs */
        if (userin(prompt) == EOF)
            break;
        procline();
    }
 
    printf("\n");
    return 0;
}
