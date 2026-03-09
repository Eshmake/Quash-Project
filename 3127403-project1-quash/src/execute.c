
/**
 * @file execute.c
 *
 * @brief Implements interface functions between Quash and the environment and
 * functions that interpret an execute commands.
 *
 * @note As you add things to this file you may want to change the method signature
 */

#include "execute.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "command.h"
#include "quash.h"

#define READ_END 0
#define WRITE_END 1

/***************************************************************************
 * Data structures for job control
 ***************************************************************************/

typedef struct Job {
  int job_id;
  pid_t rep_pid;
  char* cmd;
  pid_t* pids;
  size_t pid_count;
  struct Job* next;
} Job;

typedef struct ExecState {
  pid_t pids[256];
  size_t pid_count;
  int prev_pipe_read;
} ExecState;

static Job* bg_jobs = NULL;
static ExecState exec_state = {{0},0,-1};
static bool cleanup_registered = false;

// **INTERNAL HELPERS ADDED**:

 // closes a valid fd
static void safe_close(int fd) {
  if (fd >= 0)
    close(fd);
}

// frees memory of one job
static void free_job(Job* job) {
  if (!job) return;
  free(job->cmd);
  free(job->pids);
  free(job);
}

// returns next available job #
static int next_job_id(void) {
  int max = 0;
  for (Job* j = bg_jobs; j; j = j->next)
    if (j->job_id > max)
      max = j->job_id;
  return max + 1;
}

// frees all job memory when shell exits
static void destroy_bg_jobs(void) {
  Job* cur = bg_jobs;
  while (cur) {
    Job* next = cur->next;

    for (size_t i=0;i<cur->pid_count;i++) {
      if (kill(cur->pids[i], SIGTERM) != 0 && errno != ESRCH)
        perror("kill");
    }

    for (size_t i=0;i<cur->pid_count;i++) {
      int status;
      while (waitpid(cur->pids[i], &status, 0) < 0) {
        if (errno == EINTR) continue;
        if (errno == ECHILD) break;
        break;
      }
    }

    free_job(cur);
    cur = next;
  }
  bg_jobs = NULL;
}

// ensures bg jobs are cleaned upon exit
static void ensure_cleanup_registered(void) {
  if (!cleanup_registered) {
    atexit(destroy_bg_jobs);
    cleanup_registered = true;
  }
}

// returns whether a cmd is built-in (as such can not only be handled inside child)
static bool is_parent_builtin(CommandType type) {
  return type == EXPORT || type == CD || type == KILL || type == EXIT;
}

// checks whether current cmd line is simple one-cmd built-in
static bool is_simple_parent_builtin(CommandHolder* holders) {
  if (!holders) return false;
  if (get_command_holder_type(holders[0]) == EOC) return false;
  if (get_command_holder_type(holders[1]) != EOC) return false;

  if (holders[0].flags & (PIPE_IN|PIPE_OUT|REDIRECT_IN|REDIRECT_OUT|BACKGROUND))
    return false;

  return is_parent_builtin(get_command_holder_type(holders[0]));
}

// waits for one child
static void wait_for_pid(pid_t pid) {
  int status;
  while (waitpid(pid,&status,0) < 0) {
    if (errno != EINTR)
      break;
  }
}

// handles redirections
static void apply_redirections(CommandHolder holder) {

  if (holder.flags & REDIRECT_IN) {
    int fd = open(holder.redirect_in, O_RDONLY);
    if (fd < 0) { perror("open"); _exit(EXIT_FAILURE); }

    if (dup2(fd, STDIN_FILENO) < 0) {
      perror("dup2");
      close(fd);
      _exit(EXIT_FAILURE);
    }
    close(fd);
  }

  if (holder.flags & REDIRECT_OUT) {

    int flags = O_WRONLY|O_CREAT;
    if (holder.flags & REDIRECT_APPEND)
      flags |= O_APPEND;
    else
      flags |= O_TRUNC;

    int fd = open(holder.redirect_out, flags, 0644);
    if (fd < 0) { perror("open"); _exit(EXIT_FAILURE); }

    if (dup2(fd, STDOUT_FILENO) < 0) {
      perror("dup2");
      close(fd);
      _exit(EXIT_FAILURE);
    }
    close(fd);
  }
}

// makes new Job node (in LL) for bg cmd
static void add_bg_job(const pid_t* pids, size_t pid_count) {

  Job* job = malloc(sizeof(Job));
  if (!job) return;

  job->job_id = next_job_id();
  job->rep_pid = pids[0];
  job->cmd = get_command_string();
  job->pids = malloc(sizeof(pid_t)*pid_count);
  job->pid_count = pid_count;
  job->next = NULL;

  for (size_t i=0;i<pid_count;i++)
    job->pids[i] = pids[i];

  if (!bg_jobs)
    bg_jobs = job;
  else {
    Job* cur = bg_jobs;
    while (cur->next) cur = cur->next;
    cur->next = job;
  }

  //prints bg job start string
  print_job_bg_start(job->job_id, job->rep_pid, job->cmd);
}

/***************************************************************************
 * Interface Functions
 ***************************************************************************/


// Return a string containing the current working directory.
char* get_current_directory(bool* should_free) {

  char* cwd = getcwd(NULL,0);

  if (!cwd) {
    *should_free = false;
    return "";
  }

  *should_free = true;
  return cwd;
}


// Returns the value of an environment variable env_var
const char* lookup_env(const char* env_var) {

  const char* val = getenv(env_var);
  return val ? val : "";
}

// writes an env variable
void write_env(const char* env_var, const char* val) {

  if (!env_var) return;

  if (setenv(env_var, val?val:"", 1) != 0)
    perror("setenv");
}

// Check the status of background jobs
void check_jobs_bg_status() {

  Job* cur = bg_jobs;
  Job* prev = NULL;

  while (cur) {

    bool done = true;

    for (size_t i=0;i<cur->pid_count;i++) {
      int status;
      pid_t rc = waitpid(cur->pids[i], &status, WNOHANG);

      if (rc == 0)
        done = false;
      else if (rc < 0 && errno != ECHILD)
        done = false;
    }

    if (done) {

      Job* finished = cur;

      if (!prev)
        bg_jobs = cur->next;
      else
        prev->next = cur->next;

      cur = cur->next;

      print_job_bg_complete(
        finished->job_id,
        finished->rep_pid,
        finished->cmd);

      free_job(finished);

    } else {
      prev = cur;
      cur = cur->next;
    }
  }
}

// Prints the job id number, the process id of the first process belonging to
// the Job, and the command string associated with this job
void print_job(int job_id, pid_t pid, const char* cmd) {
  printf("[%d]\t%d\t%s\n", job_id, pid, cmd);
  fflush(stdout);
}

// Prints a start up message for background processes
void print_job_bg_start(int job_id, pid_t pid, const char* cmd) {
  printf("Background job started: ");
  print_job(job_id, pid, cmd);
}

// Prints a completion message followed by the print job
void print_job_bg_complete(int job_id, pid_t pid, const char* cmd) {
  printf("Completed: ");
  print_job(job_id, pid, cmd);
}

/***************************************************************************
 * Functions to process commands
 ***************************************************************************/

// Run a program reachable by the path environment variable, relative path, or
// absolute path
void run_generic(GenericCommand cmd) {

  execvp(cmd.args[0], cmd.args);

  perror("execvp");
  _exit(EXIT_FAILURE);
}

// Print strings
void run_echo(EchoCommand cmd) {

  if (cmd.args) {
    for (size_t i=0; cmd.args[i]; i++) {
      if (i>0) putchar(' ');
      fputs(cmd.args[i], stdout);
    }
  }

  putchar('\n');
  fflush(stdout);
}

// set env variable
void run_export(ExportCommand cmd) {
  write_env(cmd.env_var, cmd.val);
}

// Changes the current working directory
void run_cd(CDCommand cmd) {

  const char* target = cmd.dir;

  if (!target || !target[0])
    target = lookup_env("HOME");

  char* resolved = realpath(target,NULL);
  if (!resolved) { perror("realpath"); return; }

  if (chdir(resolved) != 0) {
    perror("chdir");
    free(resolved);
    return;
  }

  write_env("PWD", resolved);
  free(resolved);
}

// print actual cwd
void run_pwd() {

  char* cwd = getcwd(NULL,0);
  if (!cwd) { perror("getcwd"); return; }

  puts(cwd);
  fflush(stdout);
  free(cwd);
}

// prints all running bg jobs
void run_jobs() {

  for (Job* j=bg_jobs; j; j=j->next)
    print_job(j->job_id, j->rep_pid, j->cmd);
}

// Sends a signal to all processes contained in a job
void run_kill(KillCommand cmd) {

  Job* cur = bg_jobs;
  Job* prev = NULL;

  while (cur) {

    if (cur->job_id == cmd.job) {

      for (size_t i=0;i<cur->pid_count;i++)
        if (kill(cur->pids[i], cmd.sig) != 0 && errno != ESRCH)
          perror("kill");

      for (size_t i=0;i<cur->pid_count;i++)
        wait_for_pid(cur->pids[i]);

      print_job_bg_complete(cur->job_id, cur->rep_pid, cur->cmd);

      if (!prev)
        bg_jobs = cur->next;
      else
        prev->next = cur->next;

      free_job(cur);
      return;
    }

    prev = cur;
    cur = cur->next;
  }
}

/***************************************************************************
 * Functions for command resolution and process setup
 ***************************************************************************/

/**
 * @brief A dispatch function to resolve the correct @a Command variant
 * function for child processes.
 *
 * This version of the function is tailored to commands that should be run in
 * the child process of a fork.
 *
 * @param cmd The Command to try to run
 *
 * @sa Command
 */

 // decides cmd runner based on cmd type for forked child
void child_run_command(Command cmd) {

  switch (get_command_type(cmd)) {

  case GENERIC: run_generic(cmd.generic); break;
  case ECHO: run_echo(cmd.echo); _exit(EXIT_SUCCESS);
  case PWD: run_pwd(); _exit(EXIT_SUCCESS);
  case JOBS: run_jobs(); _exit(EXIT_SUCCESS);
  case EXPORT: run_export(cmd.export); _exit(EXIT_SUCCESS);
  case CD: run_cd(cmd.cd); _exit(EXIT_SUCCESS);
  case KILL: run_kill(cmd.kill); _exit(EXIT_SUCCESS);
  case EXIT: _exit(EXIT_SUCCESS);

  default:
    _exit(EXIT_FAILURE);
  }
}

/**
 * @brief A dispatch function to resolve the correct @a Command variant
 * function for the quash process.
 *
 * This version of the function is tailored to commands that should be run in
 * the parent process (quash).
 *
 * @param cmd The Command to try to run
 *
 * @sa Command
 */

// run cmds that change shell itself (and req parent participation)
void parent_run_command(Command cmd) {

  switch (get_command_type(cmd)) {

  case EXPORT: run_export(cmd.export); break;
  case CD: run_cd(cmd.cd); break;
  case KILL: run_kill(cmd.kill); break;

  case EXIT:
    destroy_bg_jobs();
    end_main_loop();
    break;

  default:
    break;
  }
}

/**
 * @brief Creates one new process centered around the @a Command in the @a
 * CommandHolder setting up redirects and pipes where needed
 *
 * @note Processes are not the same as jobs. A single job can have multiple
 * processes running under it. This function creates a process that is part of a
 * larger job.
 *
 * @note Not all commands should be run in the child process. A few need to
 * change the quash process in some way
 *
 * @param holder The CommandHolder to try to run
 *
 * @sa Command CommandHolder
 */

void create_process(CommandHolder holder) {

  bool p_in  = holder.flags & PIPE_IN;
  bool p_out = holder.flags & PIPE_OUT;

  int pipe_fd[2] = {-1,-1};

  if (p_out && pipe(pipe_fd) < 0) {
    perror("pipe");
    return;
  }

  pid_t pid = fork();

  if (pid < 0) {
    perror("fork");
    return;
  }

  if (pid == 0) {

    if (p_in && exec_state.prev_pipe_read >= 0)
      dup2(exec_state.prev_pipe_read, STDIN_FILENO);

    if (p_out)
      dup2(pipe_fd[WRITE_END], STDOUT_FILENO);

    safe_close(exec_state.prev_pipe_read);
    safe_close(pipe_fd[READ_END]);
    safe_close(pipe_fd[WRITE_END]);

    apply_redirections(holder);

    child_run_command(holder.cmd);

    _exit(EXIT_FAILURE);
  }

  if (exec_state.pid_count < 256)
    exec_state.pids[exec_state.pid_count++] = pid;

  safe_close(exec_state.prev_pipe_read);
  safe_close(pipe_fd[WRITE_END]);

  exec_state.prev_pipe_read = p_out ? pipe_fd[READ_END] : -1;
}

// Run a list of commands
void run_script(CommandHolder* holders) {

  // ensure bg jobs are cleaned on exit
  ensure_cleanup_registered();

  if (!holders)
    return;

  // check whether any bg job is done
  check_jobs_bg_status();

  // handle empty cmd
  if (get_command_holder_type(holders[0]) == EOC)
    return;

  // run simple parent cmds
  if (is_simple_parent_builtin(holders)) {
    parent_run_command(holders[0].cmd);
    return;
  }


  exec_state.pid_count = 0;
  exec_state.prev_pipe_read = -1;

  // loop thru parsed cmds and build entire job
  for (int i=0; get_command_holder_type(holders[i]) != EOC; i++)
    create_process(holders[i]);

  // close last pipe read-end
  safe_close(exec_state.prev_pipe_read);


  if (exec_state.pid_count == 0)
    return;

  // wait for all child processes if non-bg job
  if (!(holders[0].flags & BACKGROUND)) {

    for (size_t i=0;i<exec_state.pid_count;i++)
      wait_for_pid(exec_state.pids[i]);

  } else {

    add_bg_job(exec_state.pids, exec_state.pid_count);
  }

  // check bg completions
  check_jobs_bg_status();
}

