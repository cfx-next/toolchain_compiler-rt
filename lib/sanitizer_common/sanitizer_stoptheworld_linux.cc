//===-- sanitizer_stoptheworld_linux.cc -----------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// See sanitizer_stoptheworld.h for details.
// This implementation was inspired by Markus Gutschke's linuxthreads.cc.
//
//===----------------------------------------------------------------------===//

#ifdef __linux__

#include "sanitizer_stoptheworld.h"

#include <errno.h>
#include <sched.h> // for clone
#include <stddef.h>
#include <sys/prctl.h> // for PR_* definitions
#include <sys/ptrace.h> // for PTRACE_* definitions
#include <sys/types.h> // for pid_t
#include <sys/wait.h> // for signal-related stuff

#include "sanitizer_common.h"
#include "sanitizer_libc.h"
#include "sanitizer_linux.h"
#include "sanitizer_mutex.h"

// This module works by spawning a Linux task which then attaches to every
// thread in the caller process with ptrace. This suspends the threads, and
// PTRACE_GETREGS can then be used to obtain their register state. The callback
// supplied to StopTheWorld() is run in the tracer task while the threads are
// suspended.
// The tracer task must be placed in a different thread group for ptrace to
// work, so it cannot be spawned as a pthread. Instead, we use the low-level
// clone() interface (we want to share the address space with the caller
// process, so we prefer clone() over fork()).
//
// We avoid the use of libc for two reasons:
// 1. calling a library function while threads are suspended could cause a
// deadlock, if one of the treads happens to be holding a libc lock;
// 2. it's generally not safe to call libc functions from the tracer task,
// because clone() does not set up a thread-local storage for it. Any
// thread-local variables used by libc will be shared between the tracer task
// and the thread which spawned it.
//
// We deal with this by replacing libc calls with calls to our own
// implementations defined in sanitizer_libc.h and sanitizer_linux.h. However,
// there are still some libc functions which are used here:
//
// * All of the system calls ultimately go through the libc syscall() function.
// We're operating under the assumption that syscall()'s implementation does
// not acquire any locks or use any thread-local data (except for the errno
// variable, which we handle separately).
//
// * We lack custom implementations of sigfillset() and sigaction(), so we use
// the libc versions instead. The same assumptions as above apply.
//
// * It is safe to call libc functions before the cloned thread is spawned or
// after it has exited. The following functions are used in this manner:
// sigdelset()
// sigprocmask()
// clone()

COMPILER_CHECK(sizeof(SuspendedThreadID) == sizeof(pid_t));

namespace __sanitizer {
// This class handles thread suspending/unsuspending in the tracer thread.
class ThreadSuspender {
 public:
  explicit ThreadSuspender(pid_t pid)
    : pid_(pid) {
      CHECK_GE(pid, 0);
    }
  bool SuspendAllThreads();
  void ResumeAllThreads();
  void KillAllThreads();
  SuspendedThreadsList &suspended_threads_list() {
    return suspended_threads_list_;
  }
 private:
  SuspendedThreadsList suspended_threads_list_;
  pid_t pid_;
  bool SuspendThread(SuspendedThreadID thread_id);
};

bool ThreadSuspender::SuspendThread(SuspendedThreadID thread_id) {
  // Are we already attached to this thread?
  // Currently this check takes linear time, however the number of threads is
  // usually small.
  if (suspended_threads_list_.Contains(thread_id))
    return false;
  if (internal_ptrace(PTRACE_ATTACH, thread_id, NULL, NULL) != 0) {
    // Either the thread is dead, or something prevented us from attaching.
    // Log this event and move on.
    Report("Could not attach to thread %d (errno %d).\n", thread_id, errno);
    return false;
  } else {
    if (SanitizerVerbosity > 0)
      Report("Attached to thread %d.\n", thread_id);
    // The thread is not guaranteed to stop before ptrace returns, so we must
    // wait on it.
    int waitpid_status;
    HANDLE_EINTR(waitpid_status, internal_waitpid(thread_id, NULL, __WALL));
    if (waitpid_status < 0) {
      // Got a ECHILD error. I don't think this situation is possible, but it
      // doesn't hurt to report it.
      Report("Waiting on thread %d failed, detaching (errno %d).\n", thread_id,
             errno);
      internal_ptrace(PTRACE_DETACH, thread_id, NULL, NULL);
      return false;
    }
    suspended_threads_list_.Append(thread_id);
    return true;
  }
}

void ThreadSuspender::ResumeAllThreads() {
  for (uptr i = 0; i < suspended_threads_list_.thread_count(); i++) {
    pid_t tid = suspended_threads_list_.GetThreadID(i);
    if (internal_ptrace(PTRACE_DETACH, tid, NULL, NULL) == 0) {
      if (SanitizerVerbosity > 0)
        Report("Detached from thread %d.\n", tid);
    } else {
      // Either the thread is dead, or we are already detached.
      // The latter case is possible, for instance, if this function was called
      // from a signal handler.
      Report("Could not detach from thread %d (errno %d).\n", tid, errno);
    }
  }
}

void ThreadSuspender::KillAllThreads() {
  for (uptr i = 0; i < suspended_threads_list_.thread_count(); i++)
    internal_ptrace(PTRACE_KILL, suspended_threads_list_.GetThreadID(i),
                    NULL, NULL);
}

bool ThreadSuspender::SuspendAllThreads() {
  ThreadLister thread_lister(pid_);
  bool added_threads;
  do {
    // Run through the directory entries once.
    added_threads = false;
    pid_t tid = thread_lister.GetNextTID();
    while (tid >= 0) {
      if (SuspendThread(tid))
        added_threads = true;
      tid = thread_lister.GetNextTID();
    }
    if (thread_lister.error()) {
      // Detach threads and fail.
      ResumeAllThreads();
      return false;
    }
    thread_lister.Reset();
  } while (added_threads);
  return true;
}

// Pointer to the ThreadSuspender instance for use in signal handler.
static ThreadSuspender *thread_suspender_instance = NULL;

// Signals that should not be blocked (this is used in the parent thread as well
// as the tracer thread).
static const int kUnblockedSignals[] = { SIGABRT, SIGILL, SIGFPE, SIGSEGV,
                                         SIGBUS, SIGXCPU, SIGXFSZ };

// Structure for passing arguments into the tracer thread.
struct TracerThreadArgument {
  StopTheWorldCallback callback;
  void *callback_argument;
};

// Signal handler to wake up suspended threads when the tracer thread dies.
void TracerThreadSignalHandler(int signum, siginfo_t *siginfo, void *) {
  if (thread_suspender_instance != NULL) {
    if (signum == SIGABRT)
      thread_suspender_instance->KillAllThreads();
    else
      thread_suspender_instance->ResumeAllThreads();
  }
  internal__exit((signum == SIGABRT) ? 1 : 2);
}

// The tracer thread waits on this mutex while the parent finished its
// preparations.
static BlockingMutex tracer_init_mutex(LINKER_INITIALIZED);

// Size of alternative stack for signal handlers in the tracer thread.
static const int kHandlerStackSize = 4096;

// This function will be run as a cloned task.
int TracerThread(void* argument) {
  TracerThreadArgument *tracer_thread_argument =
      (TracerThreadArgument *)argument;

  // Wait for the parent thread to finish preparations.
  tracer_init_mutex.Lock();
  tracer_init_mutex.Unlock();

  ThreadSuspender thread_suspender(internal_getppid());
  // Global pointer for the signal handler.
  thread_suspender_instance = &thread_suspender;

  // Alternate stack for signal handling.
  InternalScopedBuffer<char> handler_stack_memory(kHandlerStackSize);
  struct sigaltstack handler_stack;
  internal_memset(&handler_stack, 0, sizeof(handler_stack));
  handler_stack.ss_sp = handler_stack_memory.data();
  handler_stack.ss_size = kHandlerStackSize;
  internal_sigaltstack(&handler_stack, NULL);

  // Install our handler for fatal signals. Other signals should be blocked by
  // the mask we inherited from the caller thread.
  for (uptr signal_index = 0; signal_index < ARRAY_SIZE(kUnblockedSignals);
       signal_index++) {
    struct sigaction new_sigaction;
    internal_memset(&new_sigaction, 0, sizeof(new_sigaction));
    new_sigaction.sa_sigaction = TracerThreadSignalHandler;
    new_sigaction.sa_flags = SA_ONSTACK | SA_SIGINFO;
    sigfillset(&new_sigaction.sa_mask);
    sigaction(kUnblockedSignals[signal_index], &new_sigaction, NULL);
  }

  int exit_code = 0;
  if (!thread_suspender.SuspendAllThreads()) {
    Report("Failed suspending threads.\n");
    exit_code = 3;
  } else {
    tracer_thread_argument->callback(thread_suspender.suspended_threads_list(),
                                     tracer_thread_argument->callback_argument);
    thread_suspender.ResumeAllThreads();
    exit_code = 0;
  }
  thread_suspender_instance = NULL;
  handler_stack.ss_flags = SS_DISABLE;
  internal_sigaltstack(&handler_stack, NULL);
  return exit_code;
}

static BlockingMutex stoptheworld_mutex(LINKER_INITIALIZED);

void StopTheWorld(StopTheWorldCallback callback, void *argument) {
  BlockingMutexLock lock(&stoptheworld_mutex);
  // Block all signals that can be blocked safely, and install default handlers
  // for the remaining signals.
  // We cannot allow user-defined handlers to run while the ThreadSuspender
  // thread is active, because they could conceivably call some libc functions
  // which modify errno (which is shared between the two threads).
  sigset_t blocked_sigset;
  sigfillset(&blocked_sigset);
  struct sigaction old_sigactions[ARRAY_SIZE(kUnblockedSignals)];
  for (uptr signal_index = 0; signal_index < ARRAY_SIZE(kUnblockedSignals);
       signal_index++) {
    // Remove the signal from the set of blocked signals.
    sigdelset(&blocked_sigset, kUnblockedSignals[signal_index]);
    // Install the default handler.
    struct sigaction new_sigaction;
    internal_memset(&new_sigaction, 0, sizeof(new_sigaction));
    new_sigaction.sa_handler = SIG_DFL;
    sigfillset(&new_sigaction.sa_mask);
    sigaction(kUnblockedSignals[signal_index], &new_sigaction,
                    &old_sigactions[signal_index]);
  }
  sigset_t old_sigset;
  int sigprocmask_status = sigprocmask(SIG_BLOCK, &blocked_sigset, &old_sigset);
  CHECK_EQ(sigprocmask_status, 0); // sigprocmask should never fail
  // Make this process dumpable. Processes that are not dumpable cannot be
  // attached to.
  int process_was_dumpable = internal_prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
  if (!process_was_dumpable)
    internal_prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
  // Block the execution of TracerThread until after we have set ptrace
  // permissions.
  tracer_init_mutex.Lock();
  // Prepare the arguments for TracerThread.
  struct TracerThreadArgument tracer_thread_argument;
  tracer_thread_argument.callback = callback;
  tracer_thread_argument.callback_argument = argument;
  // The tracer thread will run on the same stack, so we must reserve some
  // stack space for the caller thread to run in as it waits on the tracer.
  const uptr kReservedStackSize = 4096;
  // Get a 16-byte aligned pointer for stack.
  int a_local_variable __attribute__((__aligned__(16)));
  pid_t tracer_pid = clone(TracerThread,
                          (char *)&a_local_variable - kReservedStackSize,
                          CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_UNTRACED,
                          &tracer_thread_argument, 0, 0, 0);
  if (tracer_pid < 0) {
    Report("Failed spawning a tracer thread (errno %d).\n", errno);
    tracer_init_mutex.Unlock();
  } else {
    // On some systems we have to explicitly declare that we want to be traced
    // by the tracer thread.
#ifdef PR_SET_PTRACER
    internal_prctl(PR_SET_PTRACER, tracer_pid, 0, 0, 0);
#endif
    // Allow the tracer thread to start.
    tracer_init_mutex.Unlock();
    // Since errno is shared between this thread and the tracer thread, we
    // must avoid using errno while the tracer thread is running.
    // At this point, any signal will either be blocked or kill us, so waitpid
    // should never return (and set errno) while the tracer thread is alive.
    int waitpid_status = internal_waitpid(tracer_pid, NULL, __WALL);
    if (waitpid_status < 0)
      Report("Waiting on the tracer thread failed (errno %d).\n", errno);
  }
  // Restore the dumpable flag.
  if (!process_was_dumpable)
    internal_prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
  // Restore the signal handlers.
  for (uptr signal_index = 0; signal_index < ARRAY_SIZE(kUnblockedSignals);
       signal_index++) {
    sigaction(kUnblockedSignals[signal_index],
              &old_sigactions[signal_index], NULL);
  }
  sigprocmask(SIG_SETMASK, &old_sigset, &old_sigset);
}

}  // namespace __sanitizer

#endif  // __linux__