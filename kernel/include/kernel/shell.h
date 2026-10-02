/* StratumOS - interactive shell.
 *
 * Reads from the keyboard ring buffer (which the serial driver also feeds), so
 * the same shell is usable over a serial console. Supports line editing,
 * command history and argument parsing.
 */
#ifndef _KERNEL_SHELL_H
#define _KERNEL_SHELL_H

#include <kernel/types.h>

#define SHELL_LINE_MAX 256
#define SHELL_MAX_ARGS 16
#define SHELL_HISTORY  16

struct shell_command {
    const char *name;
    const char *usage;
    const char *help;
    int (*fn)(int argc, char **argv);
};

void shell_init(void);
NORETURN void shell_task(void *arg);
void shell_run_line(const char *line);

#endif /* _KERNEL_SHELL_H */
