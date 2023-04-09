#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "builtin.h"
#include "parse.h"

static char *builtin[] = {
    "exit",  /* exits the shell */
    "which", /* displays full path to command */
    "jobs",  // Display all current jobs
    "kill",  //
    "fg",    // bring a process to fg
    "bg",    // bring a process to bg
    NULL};

int is_builtin(char *cmd)
{
    int i;

    for (i = 0; builtin[i]; i++)
    {
        if (!strcmp(cmd, builtin[i]))
            return 1;
    }

    return 0;
}