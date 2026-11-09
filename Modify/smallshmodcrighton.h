/* Base version from the textbook (Section 5.9) + Phase 1 additions
 * (lines marked NEW). */
 
#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <stdlib.h>   /* exit() */
#include <signal.h>   /* NEW: sigaction, SIGINT, SIGTSTP, ... */
#include <errno.h>    /* NEW: errno, EINTR */
#include <string.h>   /* NEW: strlen() in the SIGINT handler */
#include <sys/stat.h> /* NEW: sys/stat.h for handling mkdir function */
 
#define EOL         1   /* end of line */
#define ARG         2   /* normal arguments */
#define AMPERSAND   3
#define SEMICOLON   4
 
#define MAXARG    512   /* max. no. command args */
#define MAXBUF    512   /* max. length input line */
#define MAXJOBS   64    /* max. no. of jobs*/
 
#define FOREGROUND  0
#define BACKGROUND  1
 
/* Original routines */
int userin(char *p);
int gettok(char **outptr);
int inarg(char c);
int procline(void);
int runcommand(char **cline, int where);
 
/* NEW: signal handling and foreground/background management */
void init_shell(void);          /* set up shell signals + its process group */
void reap_background(void);     /* collect finished background jobs */
 
