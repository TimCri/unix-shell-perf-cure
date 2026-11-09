#include "smallsh.h"
 
/* program buffers and work pointers */
static char inpbuf[MAXBUF], tokbuf[2 * MAXBUF],
            *ptr = inpbuf, *tok = tokbuf;
 
char *prompt = "Command> ";   /* prompt */
 
/* NEW: job-control state */
static pid_t shell_pgid;      /* process group the shell runs in        */
static int   interactive;     /* 1 if stdin is a terminal, 0 otherwise  */
 
/* NEW: job table */
struct job {
    pid_t pid;                /* 0 means the slot is empty   */
    char  status[16];         /* "Running" or "Stopped"      */
};
static struct job jobtable[MAXJOBS];
 
/* ------------------------------------------------------------------ */
/* NEW: signal handling                                                */
/* ------------------------------------------------------------------ */
 
/* NEW: count of Ctrl+C presses in a row at the prompt.
 * volatile sig_atomic_t is the only type that is safe to change
 * inside a signal handler and read in normal code. */
static volatile sig_atomic_t sigint_count = 0;
 
/* NEW: the shell's own SIGINT handler.
 * Ctrl+C at the prompt does NOT kill the shell; it gives a fresh prompt.
 * Three presses in a row exit the shell.
 * Only async-signal-safe calls (write, strlen, _exit) are allowed here,
 * so no printf. */
static void sigint_handler(int sig)
{
    static const char msg[] =
        "\n(3 Ctrl+C presses received. Exiting smallsh...)\n";
    (void)sig;
 
    sigint_count++;
    if (sigint_count >= 3) {
        write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        _exit(0);
    }
 
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
/* NEW: job table                                                      */
/* ------------------------------------------------------------------ */
 
/* Add a job to the first empty slot */
void add_job(pid_t pid, const char *status)
{
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobtable[i].pid == 0) {
            jobtable[i].pid = pid;
            snprintf(jobtable[i].status, sizeof(jobtable[i].status),
                     "%s", status);
            return;
        }
    }
    fprintf(stderr, "smallsh: job table full, pid %d not tracked\n", pid);
}
 
/* Update a job's status so jobs, fg, and bg can see it */
void update_job_status(pid_t pid, const char *status)
{
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobtable[i].pid == pid) {
            snprintf(jobtable[i].status, sizeof(jobtable[i].status),
                     "%s", status);
            return;
        }
    }
}
 
/* Remove a job from the table */
void remove_job(pid_t pid)
{
    for (int i = 0; i < MAXJOBS; i++) {
        if (jobtable[i].pid == pid) {
            jobtable[i].pid = 0;
            jobtable[i].status[0] = '\0';
            return;
        }
    }
}
 
/* Is this pid in the job table? */
static int find_job(pid_t pid)
{
    for (int i = 0; i < MAXJOBS; i++)
        if (jobtable[i].pid == pid)
            return 1;
    return 0;
}
 
/* ------------------------------------------------------------------ */
/* NEW: foreground / background process management                     */
/* ------------------------------------------------------------------ */
 
/* NEW: wait for one foreground job.
 * WUNTRACED makes waitpid also return if the job is STOPPED (Ctrl+Z),
 * otherwise the shell would hang forever waiting on a stopped process.
 * Used both by runcommand and by the fg built-in. */
static int wait_foreground(pid_t pid)
{
    int status;
    pid_t r;
 
    do {
        r = waitpid(pid, &status, WUNTRACED);
    } while (r == -1 && errno == EINTR);
 
    if (r == -1) {
        perror("smallsh: waitpid");
        remove_job(pid);
        return (-1);
    }
 
    if (WIFSTOPPED(status)) {
        printf("\n[Process id %d] Stopped\n", pid);
        update_job_status(pid, "Stopped");     /* keep it for fg/bg */
    } else {
        /* exited or killed: it's gone, so take it out of the table */
        if (WIFSIGNALED(status)) {
            if (WTERMSIG(status) == SIGINT)
                printf("\n");   /* Ctrl+C: just move to a clean line */
            else
                printf("[Process id %d] Terminated by signal %d\n",
                       pid, WTERMSIG(status));
        }
        remove_job(pid);
    }
 
    return status;
}
 
/* NEW: run a job in the foreground: give it the terminal, wait for it,
 * then take the terminal back for the shell. */
static int foreground_job(pid_t pid)
{
    int status;
 
    if (interactive)
        tcsetpgrp(STDIN_FILENO, pid);
 
    status = wait_foreground(pid);
 
    if (interactive)
        tcsetpgrp(STDIN_FILENO, shell_pgid);
 
    return status;
}
 
/* NEW: collect any background jobs that have finished.
 * WNOHANG = don't block if nobody has finished yet.
 * Called before each prompt, so finished jobs don't stay zombies. */
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
        remove_job(pid);
    }
}
 
/* ------------------------------------------------------------------ */
/* NEW: built-in commands                                              */
/* ------------------------------------------------------------------ */
 
/* Pick the most recent job (optionally only a stopped one) */
static pid_t latest_job(int stopped_only)
{
    for (int i = MAXJOBS - 1; i >= 0; i--) {
        if (jobtable[i].pid != 0 &&
            (!stopped_only || strcmp(jobtable[i].status, "Stopped") == 0))
            return jobtable[i].pid;
    }
    return 0;
}
 
/* Returns 1 if cline was a built-in (and it has been handled),
 * 0 if it is an ordinary command for runcommand(). */
int is_builtin(char **cline)
{
    if (cline[0] == NULL)
        return 0;
 
    /* --- BUILT-IN: cd --- */
    if (strcmp(cline[0], "cd") == 0) {
        char *target = cline[1];
        if (target == NULL) {
            target = getenv("HOME");
            if (target == NULL) {
                fprintf(stderr, "smallsh: cd: HOME not set\n");
                return 1;
            }
        }
        if (chdir(target) != 0)
            perror("smallsh: cd");
        return 1;
    }
 
    /* --- BUILT-IN: mkdir --- */
    if (strcmp(cline[0], "mkdir") == 0) {
        if (cline[1] == NULL) {
            fprintf(stderr, "smallsh: mkdir: missing operand\n");
            return 1;
        }
        if (mkdir(cline[1], 0777) != 0)   /* 0777 is reduced by umask */
            perror("smallsh: mkdir");
        return 1;
    }
 
    /* --- BUILT-IN: jobs --- */
    if (strcmp(cline[0], "jobs") == 0) {
        reap_background();     /* drop jobs that just finished first */
        for (int i = 0; i < MAXJOBS; i++) {
            if (jobtable[i].pid != 0)
                printf("[%d] PID %d %s\n",
                       i + 1, jobtable[i].pid, jobtable[i].status);
        }
        return 1;
    }
 
    /* --- BUILT-IN: fg [pid] --- */
    if (strcmp(cline[0], "fg") == 0) {
        pid_t pid = (cline[1] == NULL) ? latest_job(0) : atoi(cline[1]);
 
        if (pid <= 0 || !find_job(pid)) {
            fprintf(stderr, "smallsh: fg: no such job\n");
            return 1;
        }
 
        printf("fg %d\n", pid);
        update_job_status(pid, "Running");
 
        /* Hand over the terminal BEFORE continuing the job, then
         * send SIGCONT to its whole process group (negative pid). */
        if (interactive)
            tcsetpgrp(STDIN_FILENO, pid);
        if (kill(-pid, SIGCONT) < 0) {
            perror("smallsh: fg");
            if (interactive)
                tcsetpgrp(STDIN_FILENO, shell_pgid);
            return 1;
        }
 
        foreground_job(pid);   /* waits, handles Stopped/Done, takes tty back */
        return 1;
    }
 
    /* --- BUILT-IN: bg [pid] --- */
    if (strcmp(cline[0], "bg") == 0) {
        pid_t pid = (cline[1] == NULL) ? latest_job(1) : atoi(cline[1]);
 
        if (pid <= 0 || !find_job(pid)) {
            fprintf(stderr, "smallsh: bg: no stopped job\n");
            return 1;
        }
 
        if (kill(-pid, SIGCONT) < 0) {    /* continue the whole group */
            perror("smallsh: bg");
            return 1;
        }
 
        update_job_status(pid, "Running");
        printf("[Process id %d] Running\n", pid);
        return 1;
    }
 
    /* --- BUILT-IN: kill <pid> --- */
    if (strcmp(cline[0], "kill") == 0) {
        if (cline[1] == NULL) {
            fprintf(stderr, "smallsh: kill: usage: kill <pid>\n");
            return 1;
        }
        pid_t pid = atoi(cline[1]);
        if (pid <= 0) {
            fprintf(stderr, "smallsh: kill: invalid pid\n");
            return 1;
        }
        if (kill(pid, SIGTERM) < 0) {
            perror("smallsh: kill");
            return 1;
        }
        /* A STOPPED process doesn't act on SIGTERM until it runs again,
         * so wake it up too. reap_background() reports it next prompt. */
        kill(pid, SIGCONT);
        return 1;
    }
 
    return 0;
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
 
        sigint_count = 0;    /* NEW: user typed something, reset Ctrl+C count */
 
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
                if (!is_builtin(arg))      /* NEW: built-ins run in the shell */
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
 
    /* NEW: every job goes in the table (foreground too, so Ctrl+Z works) */
    add_job(pid, "Running");
 
    /* if background process print pid and return */
    if (where == BACKGROUND)
    {
        printf("[Process id %d]\n", pid);
        return (0);
    }
 
    /* NEW: foreground: give it the terminal, wait, take the terminal back */
    return foreground_job(pid);
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
