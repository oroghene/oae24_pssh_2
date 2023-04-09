#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <readline/readline.h>
#include <errno.h>
#include "builtin.h"
#include "parse.h"
#include <sys/wait.h>
#include <fcntl.h>

/*******************************************
 * Set to 1 to view the command line parse *
 *******************************************/
#define DEBUG_PARSE 0
#define MAX_BUF 1024
#define MAX_JOBS 100

typedef enum
{
    STOPPED,
    TERM,
    BG,
    FG,
} JobStatus;

typedef struct
{
    char name[MAX_BUF];
    int job_id;
    pid_t *pids;
    unsigned int npids;
    pid_t pgid;
    JobStatus status;
} Job;

Job *jobs[MAX_JOBS]; // array to store the jobs structures
int job_num = 0;     // keep track of total jobs number
int our_tty;         // store the terminal

// Job API functions
int remove_child(int chld_pid);
int check_job_status(int pgid);
void delete_job(Job *job);
void change_job_status(int pgid, int status);
Job *create_job(int npids, int pgid, int *pids, int is_bg, char *name, int job_id);
int check_free_job();
void print_new_bg_job(Job *job);

// Builtin commands functions
int get_job_pgid(char *job_id);
int check_pid(int pid);
void kill_builtin(char **argv);
void fg(char **argv);
void bg(char **argv);
void print_job(Job *job);

void print_banner()
{
    printf("                    ________   \n");
    printf("_________________________  /_  \n");
    printf("___  __ \\_  ___/_  ___/_  __ \\ \n");
    printf("__  /_/ /(__  )_(__  )_  / / / \n");
    printf("_  .___//____/ /____/ /_/ /_/  \n");
    printf("/_/ Type 'exit' or ctrl+c to quit\n\n");
}

// Sets fg pg
void set_fg_pgrp(pid_t pgrp)
{
    void (*sav)(int sig);

    if (pgrp == 0)
        pgrp = getpgrp();

    sav = signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(our_tty, pgrp);
    signal(SIGTTOU, sav);
}

void handler(int sig)
{
    pid_t chld;
    int status;

    switch (sig)
    {

    case SIGTTOU:
        // printf("Parent: SIGTTOU RECIEVED % d\n", getpgrp());
        while (tcgetpgrp(STDOUT_FILENO) != getpgrp())
        { // pause until parent has the foreground back
            pause();
        }
        break;

    case SIGCHLD:

        while ((chld = waitpid(-1, &status, WNOHANG | WCONTINUED | WUNTRACED)) > 0)
        { // wait on children

            if (WIFCONTINUED(status))
            {
                change_job_status(getpgid(chld), 2);
            }
            else if (WIFSTOPPED(status))
            {
                change_job_status(getpgid(chld), 0);
                set_fg_pgrp(0);
            }
            else
            {
                /* waited on terminated child */

                int pgid = remove_child(chld); // removes child and returns the group id of the job
                if (pgid == -1)
                {
                    printf("Error when removing a child from a job structure. \n");
                }
                if (!check_job_status(pgid))
                {
                    set_fg_pgrp(0);
                    // add condition to check for bg
                    int i = 0;
                    while (jobs[i]->pgid != pgid)
                    {
                        ++i;
                    }
                    // if (jobs[i]->name[strlen(jobs[i]->name - 1)] == '&')
                    printf("\n[%i] + done	%s\n", jobs[i]->job_id, jobs[i]->name);
                    // fflush(stdout);
                }
            }
        }
        break;

    default:
        break;
    }
}

/* returns a string for building the prompt
 *
 * Note:
 *   If you modify this function to return a string on the heap,
 *   be sure to free() it later when appropirate!  */
static char *build_prompt(char *cwd) // Modified to get the pwd, adds "$ " and returns
{
    if (getcwd(cwd, MAX_BUF) != NULL)
    {
        strcat(cwd, "$ ");
        return cwd;
    }
    else
    {
        perror("getcwd() error");
        exit(EXIT_FAILURE);
    }
}

/* return true if command is found, either:
 *   - a valid fully qualified path was supplied to an existing file
 *   - the executable file was found in the system's PATH
 * false is returned otherwise */
char *store_path;
static int command_found(const char *cmd)
{
    char *dir;
    char *tmp;
    char *PATH;
    char *state;
    char probe[PATH_MAX];

    int ret = 0;

    // access()  checks  whether  the  calling process can access the file pathname.
    if (access(cmd, X_OK) == 0)
        return 1;

    // getenv() searches the environment list to find the environment variable name, and returns a pointer to the corresponding value string.
    PATH = strdup(getenv("PATH"));

    for (tmp = PATH;; tmp = NULL)
    {
        // strtok() breaks a string into a sequence of zero or more nonempty tokens
        dir = strtok_r(tmp, ":", &state);
        if (!dir)
            break;

        strncpy(probe, dir, PATH_MAX - 1);
        strncat(probe, "/", PATH_MAX - 1);
        strncat(probe, cmd, PATH_MAX - 1);
        store_path = probe; // store the path of the executable (used in which command)
        // F_OK tests for the existence of the file.  R_OK, W_OK, and X_OK test whether the file exists and grants read, write, and execute permissions, respectively.
        if (access(probe, X_OK) == 0)
        {
            ret = 1;
            break;
        }
    }
    free(PATH);
    return ret;
}

/*Takes a command, argv, in and out file descriptors.
Uses the command and the arguments to execute the command.
The in and out file descriptors are set accordingly to accomodate any files/pipes/stdout/stdin etc...
Supports the bultin commands which and exit.*/
int exec_cmd(char *cmd, char **options, int pip_read, int pip_write, int num, pid_t *pid_0, int bg)
{
    pid_t pid;

    pid = vfork();
    if (pid < 0)
    {
        printf("failed to fork\n");
        exit(EXIT_FAILURE);
    }

    // Add forked children into their process groups
    if (num == 0)
    {                         // this is the first forked child
        *pid_0 = pid;         // save its pid as a PGID
        setpgid(pid, *pid_0); // place first child in its own group
    }
    else
    {
        setpgid(pid, *pid_0); // place new child in the group of the first child
    }
    if (bg)
    {
        set_fg_pgrp(0);
    }
    else
    {
        set_fg_pgrp(*pid_0); // set foregorund process group
    }

    if (pid == 0)
    { // Child Process - executes command

        if (pip_read != STDIN_FILENO)
        {
            if (dup2(pip_read, STDIN_FILENO) == -1)
            {
                printf("failed to dup!\n");
                exit(EXIT_FAILURE);
            }
        }
        if (pip_write != STDOUT_FILENO)
        {
            if (dup2(pip_write, STDOUT_FILENO) == -1)
            {
                printf("failed to dup!\n");
                exit(EXIT_FAILURE);
            }
        }

        if (strcmp(cmd, "which") == 0)
        { // bultin command which
            if (!(access(options[1], F_OK)) || command_found(options[1]))
            { // check if input file exists

                char *path = realpath(options[1], NULL);

                if (path == NULL)
                { // if NULL then look for the latest path returned by command_found
                    path = store_path;
                }
                execlp("echo", "echo", path, NULL);
            }
            else
            {
                if (is_builtin(options[1]))
                { // check if builtin
                    char *print_out = options[1];
                    strcat(print_out, ": shell built-in command");
                    execlp("echo", "echo", print_out, NULL);
                }
                else
                {
                    execlp("echo", "echo", "File not found!", NULL);
                }
            }
        }
        else
        {
            execvp(cmd, options);
            printf("Child: failed to exec!\n");
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        // printf("Parent  PID: %d\n", getpid());
        // printf("Parent PGRP: %d\n", getpgrp());
    }

    return pid; // return the pid of the created child process
}

/* Called upon receiving a successful parse.
 * This function is responsible for cycling through the
 * tasks, and forking, executing, etc as necessary to get
 * the job done! */

void execute_tasks(Parse *P, char *cmdline)
{
    unsigned int t = 0;
    pid_t pid_0 = 0; // store the pid of the first child
    pid_t child_pid = 0;
    int *pids = malloc(sizeof(int) * P->ntasks); //[P->ntasks]; //store the child pids

    if (command_found(P->tasks[0].cmd) || is_builtin(P->tasks[0].cmd))
    { // checks if first command is supported

        if (!strcmp(P->tasks[0].cmd, "exit"))
        { // implement builtin exit command to exit the program
            printf("Exiting pssh...\n");
            exit(EXIT_SUCCESS);
        }
        if (!strcmp(P->tasks[0].cmd, "jobs"))
        { // jobs command
            for (int i = 0; i < job_num; i++)
            {
                print_job(jobs[i]);
            }
            return; // no need to fork
        }
        if (!strcmp(P->tasks[0].cmd, "kill"))
        { // jobs command
            kill_builtin(P->tasks[0].argv);
            return; // no need to fork
        }

        if (!strcmp(P->tasks[0].cmd, "fg"))
        { // jobs command
            fg(P->tasks[0].argv);
            // printf("[%i] + continued	%s\n\n", jobnum, jobs[jobnum].name);
            return; // no need to fork
        }

        if (!strcmp(P->tasks[0].cmd, "bg"))
        { // jobs command
            bg(P->tasks[0].argv);
            return; // no need to fork
        }

        int fd;

        if (P->ntasks > 1)
        { // executes for piped commands

            int i;
            int fd_pip[2];
            int store_fd[P->ntasks * 2]; // array to store pipe file descriptors (2 for each command)

            for (i = 0; i < P->ntasks - 1; i++)
            { // goes through all piped commands except the last one

                if (pipe(fd_pip) == -1)
                {
                    fprintf(stderr, "failed to create pipe\n");
                    exit(EXIT_FAILURE);
                }

                // store all the pipe file descriptors
                store_fd[i * 2] = fd_pip[0];
                store_fd[i * 2 + 1] = fd_pip[1];

                if (i == 0)
                {
                    if (P->infile)
                    { // if there is a an input file
                        fd = open(P->infile, O_RDWR | O_CREAT, 0777);
                        child_pid = exec_cmd(P->tasks[i].cmd, P->tasks[i].argv, fd, store_fd[i * 2 + 1], i, &pid_0, P->background);
                        close(fd);
                    }
                    else
                    {
                        child_pid = exec_cmd(P->tasks[i].cmd, P->tasks[i].argv, STDIN_FILENO, store_fd[i * 2 + 1], i, &pid_0, P->background); // in, out
                    }
                }
                else
                {                                     // this is any piped command that is not the first or last one
                    close(store_fd[(i - 1) * 2 + 1]); // close my write then read
                    child_pid = exec_cmd(P->tasks[i].cmd, P->tasks[i].argv, store_fd[(i - 1) * 2], store_fd[i * 2 + 1], i, &pid_0, P->background);
                    close(store_fd[(i - 1) * 2]); // close my read
                }

                pids[i] = child_pid;
            }

            // Now run the last command of the piped commands
            close(store_fd[(i - 1) * 2 + 1]);
            if (P->outfile)
            {

                fd = open(P->outfile, O_RDWR | O_CREAT, 0777);
                child_pid = exec_cmd(P->tasks[i].cmd, P->tasks[i].argv, store_fd[(i - 1) * 2], fd, i, &pid_0, P->background);
                close(fd);
                close(store_fd[(i - 1) * 2]);
            }
            else
            {

                child_pid = exec_cmd(P->tasks[i].cmd, P->tasks[i].argv, store_fd[(i - 1) * 2], STDOUT_FILENO, i, &pid_0, P->background); // in, out
                close(store_fd[(i - 1) * 2]);
            }

            pids[P->ntasks - 1] = child_pid;
        }
        else
        { // executes single commands

            // if there is a an input/output file open it to fd
            if (P->infile && P->outfile)
            {

                int fd_in, fd_out;
                fd_in = open(P->infile, O_RDWR | O_CREAT, 0777);
                fd_out = open(P->outfile, O_RDWR | O_CREAT, 0777);
                child_pid = exec_cmd(P->tasks[0].cmd, P->tasks[0].argv, fd_in, fd_out, 0, &pid_0, P->background);
                close(fd_in);
                close(fd_out);
            }
            else if (P->infile)
            {

                fd = open(P->infile, O_RDWR | O_CREAT, 0777);
                child_pid = exec_cmd(P->tasks[0].cmd, P->tasks[0].argv, fd, STDOUT_FILENO, 0, &pid_0, P->background);
                close(fd);
            }
            else if (P->outfile)
            {

                fd = open(P->outfile, O_RDWR | O_CREAT, 0777);
                child_pid = exec_cmd(P->tasks[0].cmd, P->tasks[0].argv, STDIN_FILENO, fd, 0, &pid_0, P->background);
                close(fd);
            }
            else
            {
                child_pid = exec_cmd(P->tasks[0].cmd, P->tasks[0].argv, STDIN_FILENO, STDOUT_FILENO, 0, &pid_0, P->background);
            }

            pids[0] = child_pid;
        }

        // Create a job struct and store it in the array
        int indx = check_free_job();
        jobs[indx] = create_job(P->ntasks, pid_0, pids, P->background, cmdline, indx);

        // Initialize sig handler
        signal(SIGTTOU, handler);
        signal(SIGCHLD, handler);
    }
    else
    { // command is invalid
        printf("pssh: command not found: %s\n", P->tasks[t].cmd);
    }
}

int main(int argc, char **argv)
{
    char *cmdline;
    Parse *P;

    print_banner();

    if (isatty(STDOUT_FILENO))
    { // Store terminal
        our_tty = dup(STDERR_FILENO);
    }

    while (1)
    {
        char *buf = (char *)malloc(MAX_BUF * sizeof(char)); // takes the current working directory
        char *store_cmd = (char *)malloc(MAX_BUF * sizeof(char));

        cmdline = readline(build_prompt(buf));
        strncpy(store_cmd, cmdline, MAX_BUF);
        free(buf);

        if (!cmdline) /* EOF (ex: ctrl-d) */
            exit(EXIT_SUCCESS);

        P = parse_cmdline(cmdline);
        if (!P)
            goto next;

        if (P->invalid_syntax)
        {
            printf("pssh: invalid syntax \n");
            goto next;
        }

#if DEBUG_PARSE
        parse_debug(P);
#endif

        execute_tasks(P, store_cmd);

    next:
        parse_destroy(&P);
        free(cmdline);
        free(store_cmd);
    }
}

//========================================================BUILT IN FUNCTIONS==========================================================

// checks if a signal can be sent to the pid
int check_pid(int pid)
{
    errno = 0;
    // will only return 0 if pid exists and you would be able to send it a signal.
    // If pid isn't running as you (and you aren't root), it fails with -EPERM. (errno != 0)
    kill(pid, 0);
    if (errno != 0)
    { // custom print statement for the three cases
        printf("pssh: invalid pid: %d \n", pid);
        return 0;
    }
    return 1;
}

// returns the pgid of the supplied job number
int get_job_pgid(char *job_id)
{

    int i;
    char *job_id_chopped = job_id + 1; // removes % from char

    int job_indx = atoi(job_id_chopped);

    for (i = 0; i < job_num; i++)
    {
        if (jobs[i]->job_id == job_indx && jobs[i]->status != 1)
        {
            return jobs[i]->pgid;
        }
    }

    printf("pssh: invalid job number: %d\n", job_indx);
    return 0; // error
}

// Kill command function
void kill_builtin(char **argv)
{
    int status;
    int argc = 0;
    while (argv[argc] != NULL)
    {
        argc++;
    }

    if (argc == 1)
    {
        printf("Usage: kill [-s <signal>] <pid> | %%<job> ... \n");
        printf("\n");
    }
    else if (argc == 2)
    { // ./signal 1289 format

        pid_t pid;

        // this does not work
        if (argv[1][0] == '%')
        { // Job id was supplied
            status = get_job_pgid(argv[1]);
            if (!status)
            {
                return;
            }
            // printf("builtin kill before status: %d\n", status); // remove
            pid = status - 2 * status; // make it negative to kill all in pgid
        }
        else
        {
            pid = atoi(argv[1]);
        }

        if (!pid)
        {
            printf("pssh: invalid pid number: %d \n", pid);
            return;
        }
        if (!check_pid(pid))
            return; // pid cannot be sent a signal

        // this works for pids but not jobs (pgids)
        // printf("builtin kill negative group pid: %d\n", pid); // remove
        status = kill(pid, SIGTERM);
        // printf("builtin kill status after kill: %d\n", status); // remove
        if (status == -1)
        {
            printf("Sending signal to process %d was unsuccessful! \n", pid);
            return;
        }
    }
    else if (argc >= 4 && !strcmp(argv[1], "-s"))
    { // -->kill [-s <signal>] <pid> | %%<job>
        pid_t pid;

        int i = 3;
        while (argv[i] != NULL)
        {
            if (argv[i][0] == '%')
            { // Job id was supplied
                status = get_job_pgid(argv[3]);
                if (!status)
                {
                    return;
                }
                pid = status - 2 * status;
            }
            else
            {
                pid = atoi(argv[3]);
            }

            int signal = atoi(argv[2]);

            if (!pid)
            {
                printf("pssh: invalid pid number: %d \n", pid);
                return;
            }
            if (!check_pid(pid))
            {
                return;
            }

            status = kill(pid, signal);
            if (status == -1)
            {
                printf("Sending signal %d to process %d was unsuccessful! \n", signal, pid);
                return;
            }
            ++i;
        }
    }
    else
    {
        printf("Invalid arguments. \n");
        printf("Usage: kill [-s <signal>] <pid> | %%<job> ... \n");
    }
}

// bring background job to FG
void fg(char **argv)
{
    int status;
    int argc = 0;
    pid_t pgid;

    while (argv[argc] != NULL)
    {
        argc++;
    }

    if (argc == 1)
    {
        printf("Usage: fg %%<job number> \n");
        printf("\n");
        return;
    }
    else
    {
        if (argv[1][0] == '%')
        { // Job id was supplied
            status = get_job_pgid(argv[1]);
            if (!status)
            {
                return;
            }
            pgid = status;
        }
        else
        {
            printf("Usage: fg %%<job number> \n");
            printf("\n");
            return;
        }
        set_fg_pgrp(pgid);
        // set_fg_pgrp(0);
        // add condition to check for bg
        int i = 0;
        while (jobs[i]->pgid != pgid)
        {
            ++i;
        }
        // if (jobs[i]->name[strlen(jobs[i]->name - 1)] == '&')
        printf("\n[%i] + continued	%s\n", jobs[i]->job_id, jobs[i]->name);
        // fflush(stdout);
    }
}

// continue a bg job
void bg(char **argv)
{
    int status;
    int argc = 0;
    pid_t pgid;

    while (argv[argc] != NULL)
    {
        argc++;
    }

    if (argc == 1)
    {
        printf("Usage: fg %%<job number> \n");
        printf("\n");
        return;
    }
    else
    {
        if (argv[1][0] == '%')
        { // Job id was supplied
            status = get_job_pgid(argv[1]);
            if (!status)
            {
                return;
            }
            pgid = status;
        }
        else
        {
            printf("Usage: fg %%<job number> \n");
            printf("\n");
            return;
        }
        printf("pgid for bg %d\n", pgid);
        kill(pgid, SIGCONT);
        // int i = 0;
        // while (jobs[i]->pgid != pgid)
        // {
        //     ++i;
        // }
        // printf("\n[%i] + continued	%s\n", jobs[i]->job_id, jobs[i]->name);
    }
}

//==========================================================JOB CONTROLL FUNCTIONS====================================================

// add a new job to the job array - return a pointer to the structure
Job *create_job(int npids, int pgid, int *pids, int is_bg, char *name, int job_id)
{
    Job *job = (Job *)malloc(sizeof(Job));
    strncpy(job->name, name, MAX_BUF);
    job->npids = npids;
    job->pgid = pgid;
    job->job_id = job_id + 1;
    job->pids = pids;

    if (is_bg)
    {
        job->status = BG;
        print_new_bg_job(job);
    }
    else
    {
        job->status = FG;
    }
    return job;
}

// return the lowest free index in the jobs array
int check_free_job()
{
    int i;

    for (i = 0; i < job_num; i++)
    {
        if (jobs[i]->status == 1)
        {
            free(jobs[i]);
            return i;
        }
    }
    job_num++;
    return job_num - 1;
}

// Sets a terminated child pid to 0 in a job structure and return pgid of job
int remove_child(int chld_pid)
{
    int i, n;

    // printf("Removing child pid %d \n", chld_pid);
    for (i = 0; i < job_num; i++)
    { // go through all the jobs

        for (n = 0; n < jobs[i]->npids; n++)
        { // go through child pids of that job
            if (jobs[i]->pids[n] == chld_pid)
            {
                jobs[i]->pids[n] = 0; // set matched pid to 0;
                return jobs[i]->pgid;
            }
        }
    }
    return -1;
}

/*Checks if all children in a job have terminated
If yes = return 0
If no = return 1*/
int check_job_status(int pgid)
{
    int i, n;

    for (i = 0; i < job_num; i++)
    { // go through all the jobs

        if (jobs[i]->pgid == pgid)
        { // found the job
            for (n = 0; n < jobs[i]->npids; n++)
            { // go through child pids of that job

                if (jobs[i]->pids[n])
                {
                    return 1;
                }
                else if (n == jobs[i]->npids - 1)
                {
                    delete_job(jobs[i]);
                    return 0;
                }
            }
        }
    }
    return -1;
}

// Prints [job num] pid pid ....
void print_new_bg_job(Job *job)
{
    int i;
    printf("\n");
    printf("[%d] ", job->job_id);
    for (i = 0; i < job->npids; i++)
    {
        printf("%d ", job->pids[i]);
    }
    printf("\n");
    printf("\n");
}

// Prints job info for "jobs" command
void print_job(Job *job)
{

    if (job->status == 1)
    { // do not print terminated jobs
        return;
    }
    char *job_status;
    if (job->status == 0)
    {
        job_status = "stopped";
    }
    else if (job->status == 2 || job->status == 3)
    {
        job_status = "running";
    }
    // printf("\n");
    printf("[%d] + %s    %s \n", job->job_id, job_status, job->name);
    // printf("\n");
}

// Changes the status of the job (STOPPED/BG)
void change_job_status(int pgid, int status)
{
    int i;
    for (i = 0; i < job_num; i++)
    {
        if (jobs[i]->pgid == pgid)
        {

            jobs[i]->status = status;
            set_fg_pgrp(0);

            if (status == 2)
            {
                printf("\n[%d] + %s    %s \n", jobs[i]->job_id, "continued", jobs[i]->name);
            }
            else if (status == 0)
            {
                printf("\n[%d] + %s    %s\n", jobs[i]->job_id, "suspended", jobs[i]->name);
            }
        }
    }
}

void delete_job(Job *job)
{
    // printf("\n[%d] + done %s \n", job->job_id, job->name);
    job->status = TERM;
}