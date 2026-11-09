#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/stat.h> /* NEW: mkdir() */
#include <stdlib.h>   /* exit(), atoi(), getenv() */
#include <signal.h>   /* NEW: sigaction, SIGINT, SIGTSTP, ... */
#include <errno.h>    /* NEW: errno, EINTR */
#include <string.h>   /* NEW: strlen(), strcmp() */
 
#define EOL         1   /* end of line */
#define ARG         2   /* normal arguments */
#define AMPERSAND   3
#define SEMICOLON   4
 
#define MAXARG    512   /* max. no. command args */
#define MAXBUF    512   /* max. length input line */
 
#define FOREGROUND  0
#define BACKGROUND  1
 
#define MAXJOBS    64   /* NEW: max. entries in the job table */
 
/* Original routines */
int userin(char *p);
int gettok(char **outptr);
int inarg(char c);
int procline(void);
int runcommand(char **cline, int where);
 
/* NEW: signal handling and foreground/background management */
void init_shell(void);          /* set up shell signals + its process group */
void reap_background(void);     /* collect finished background jobs */
 
/* NEW: job table and built-in commands */
void add_job(pid_t pid, const char *status);
void update_job_status(pid_t pid, const char *status);
void remove_job(pid_t pid);
int  is_builtin(char **cline);
