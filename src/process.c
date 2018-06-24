/*  
    This file is part of Restraint.

    Restraint is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Restraint is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Restraint.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <pty.h>
#include <fcntl.h>
#include <stdio.h>
#include "process.h"

GQuark restraint_process_error (void)
{
    return g_quark_from_static_string("restraint-process-error-quark");
}

static void
process_cancelled_cb (GCancellable *cancellable, gpointer user_data);

void
process_free (ProcessData *process_data)
{
    g_cancellable_disconnect (process_data->cancellable,
                              process_data->cancel_handler);
    g_return_if_fail (process_data != NULL);
    g_clear_error (&process_data->error);
    g_strfreev (process_data->command);
    g_slice_free (ProcessData, process_data);
}

void
process_io_finish (gpointer user_data)
{
    ProcessData *process_data = (ProcessData *) user_data;

    // close the pty
    if (process_data->fd != -1 ) {
        close (process_data->fd);
        process_data->fd = -1;
    }

    // io handler is no longer active
    process_data->io_handler_id = 0;
    if (process_data->finish_handler_id == 0) {
        process_data->finish_handler_id = g_idle_add (process_pid_finish, process_data);
    }

    g_io_channel_unref(process_data->io);
}

gboolean
process_io_cb (GIOChannel *io, GIOCondition condition, gpointer user_data)
{
    ProcessData *process_data = (ProcessData *) user_data;
    return process_data->io_callback (io, condition, process_data->user_data);
}

pid_t
restraint_fork (gint *fd,
         gboolean use_pty)
{
    pid_t pid = 0;
    gint pipefd[2];
    gint devnull;

    //struct termios term;
    struct winsize win = {
        .ws_col = 80, .ws_row = 24,
        .ws_xpixel = 480, .ws_ypixel = 192,
    };

    if (use_pty) {
        pid = forkpty (fd, NULL, NULL, &win);
    } else {
        if (pipe(pipefd) == -1) {
            return -1;
       }
       pid = fork();
       if (pid == -1) {
           return -1;
       }
       if (pid == 0) {
           // Reset signal handlers.  glib2 will set SIGPIPE to ignore
           signal(SIGPIPE, SIG_DFL);

           // close reading side of pipe.
           close (pipefd[STDIN_FILENO]);

           // explicitly set /dev/null as STDIN fd
           devnull = g_open("/dev/null", O_RDONLY, 0);
           if (dup2(devnull, STDIN_FILENO) == -1) {
               // Handle dup2() error
               g_warning ("dup2 STDIN failed: %s\n", g_strerror (errno));
           }
           g_close(devnull, NULL);

           // Dupe both STDOUT and STDERR to write side of pipe
           if (dup2(pipefd[STDOUT_FILENO], STDOUT_FILENO) == -1) {
               /* Handle dup2() error */
               g_warning ("dup2 STDOUT failed: %s\n", g_strerror (errno));
           }
           if (dup2(pipefd[STDOUT_FILENO], STDERR_FILENO) == -1) {
               /* Handle dup2() error */
               g_warning ("dup2 STDERR failed: %s\n", g_strerror (errno));
           }
           // close the duped pipe
           close (pipefd[STDOUT_FILENO]);
       } else {
           // close writing side of pipe.
           close (pipefd[STDOUT_FILENO]);
           *fd = pipefd[STDIN_FILENO];
       }
    }

    return pid;
}

void
process_run (const gchar *command,
             const gchar **envp,
             const gchar *path,
             gboolean use_pty,
             guint64 max_time,
             GIOFunc io_callback,
             ProcessFinishCallback finish_callback,
             GCancellable *cancellable,
             gpointer user_data)
{
    ProcessData *process_data;
    process_data = g_slice_new0 (ProcessData);
    process_data->localwatchdog = FALSE;
    process_data->command = g_strsplit (command, " ", 0);
    process_data->path = path;
    process_data->max_time = max_time;
    process_data->io_callback = io_callback;
    process_data->finish_callback = finish_callback;
    process_data->user_data = user_data;
    process_data->io = NULL;
    process_data->cancellable = cancellable;

    if (fflush (stdout) != 0)
        g_warning ("Failed to flush stdout: %s\n", g_strerror (errno));
    if (fflush (stderr) != 0)
        g_warning ("Failed to flush stderr: %s\n", g_strerror (errno));

    process_data->pid = restraint_fork (&process_data->fd, use_pty);
    if (process_data->pid < 0) {
        /* Failed to fork */
        g_set_error (&process_data->error, RESTRAINT_PROCESS_ERROR,
                     RESTRAINT_PROCESS_FORK_ERROR,
                     "Failed to fork: %s", g_strerror (errno));
        g_idle_add (process_pid_finish, process_data);
        return;
    } else if (process_data->pid == 0) {
        /* Child process. */

        // Flush any input that hasn't been read
        if (fflush (stdin) != 0)
            g_warning ("Failed to flush stdin: %s\n", g_strerror (errno));

        setbuf (stdout, NULL);
        setbuf (stderr, NULL);
        if (process_data->path && (chdir (process_data->path) == -1)) {
            /* command_path was supplied and we failed to chdir to it. */
            g_warning ("Failed to chdir() to %s: %s\n", process_data->path, g_strerror (errno));
            exit (1);
        }
        if (envp)
            environ = (gchar **) envp;

        // Print the command being executed.
        gchar *pcommand = g_strjoinv (" ", (gchar **) process_data->command);
        g_print ("use_pty:%s %s\n", use_pty ? "TRUE" : "FALSE", pcommand);
        g_free (pcommand);

        /* Spawn the command */
        if (execvp (*process_data->command, (gchar **) process_data->command) == -1) {
            g_warning ("Failed to exec() %s, %s error:%s\n",
                       *process_data->command,
                       process_data->path, g_strerror (errno));
            exit (1);
        }
    }
    /* Parent process. */

    // If we get the cancel signal kill any running process
    if (process_data->cancellable) {
        process_data->cancel_handler = g_cancellable_connect (process_data->cancellable,
                                                              G_CALLBACK(process_cancelled_cb),
                                                              process_data,
                                                              NULL);
    }

    // close file descriptors on exec.  Should prevent leaking fd's to child processes.
    if (fcntl (process_data->fd, F_SETFD, FD_CLOEXEC) < 0) {
        g_warning("Failed to set close on exec");
    }

    // Localwatchdog handler
    if (process_data->max_time != 0) {
        process_data->timeout_handler_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                               process_data->max_time,
                                                               process_timeout_callback,
                                                               process_data,
                                                               NULL);
    }

    // IO handler
    if (io_callback != NULL) {
        GIOChannel *io = g_io_channel_unix_new (process_data->fd);
        g_io_channel_set_flags (io, G_IO_FLAG_NONBLOCK, NULL);
        // Set Encoding to NULL to keep g_io_channel from trying to decode it.
        g_io_channel_set_encoding (io, NULL, NULL);
        // Disable Buffering
        g_io_channel_set_buffered (io, FALSE);

        process_data->io = io;
        process_data->io_handler_id = g_io_add_watch_full (io,
                                                   G_PRIORITY_DEFAULT,
                                                   G_IO_IN | G_IO_HUP | G_IO_NVAL,
                                                   process_io_cb,
                                                   process_data,
                                                   process_io_finish);
    }
    // Monitor pid for return code
    process_data->pid_handler_id = g_child_watch_add_full (G_PRIORITY_DEFAULT,
                                                   process_data->pid,
                                                   process_pid_callback,
                                                   process_data,
                                                   NULL);
}

void
process_pid_callback (GPid pid, gint status, gpointer user_data)
{
    ProcessData *process_data = (ProcessData *) user_data;

    process_data->pid_result = status;
    process_data->pid = 0;
    if (process_data->fd != -1 ) {
        close (process_data->fd);
        process_data->fd = -1;
    }
    if (process_data->finish_handler_id == 0) {
        process_data->finish_handler_id = g_idle_add (process_pid_finish, process_data);
    }
}

gboolean
process_pid_finish (gpointer user_data)
{
    ProcessData *process_data = (ProcessData *) user_data;

    // If both childwatch and io_callback are finished
    // Then finish and clean ourselves up.
    if ((process_data->pid != 0) |
        (process_data->io_handler_id != 0)) {
        process_data->finish_handler_id = 0;
        return FALSE;
    }

    // Remove local watchdog handler
    if (process_data->timeout_handler_id != 0) {
        g_source_remove(process_data->timeout_handler_id);
        process_data->timeout_handler_id = 0;
    }

    process_data->finish_callback (process_data->pid_result,
                                   process_data->localwatchdog,
                                   process_data->user_data,
                                   process_data->error);

    // Free process_data.
    process_free (process_data);
    return FALSE;
}

gboolean
process_timeout_callback (gpointer user_data)
{
    ProcessData *process_data = (ProcessData *) user_data;

    process_data->timeout_handler_id = 0;

    // If pid == 0 then we are already dead
    if (process_data->pid == 0) {
        return FALSE;
    }

    // Kill process pid
    if (kill (process_data->pid, SIGKILL) == 0) {
        process_data->localwatchdog = TRUE;
    } else {
        g_warning("Local watchdog expired! But we failed to kill %i with %i", process_data->pid, SIGKILL);
        // Remove pid handler
        if (process_data->pid_handler_id) {
            g_source_remove(process_data->pid_handler_id);
            process_data->pid_handler_id = 0;
        }
    }

    return FALSE;
}

static void
process_cancelled_cb (GCancellable *cancellable, gpointer user_data)
{
    process_timeout_callback (user_data);
}
