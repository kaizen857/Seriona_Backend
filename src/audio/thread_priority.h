#pragma once

// Per-thread scheduling priority helper for the audio worker thread.
//
// Priority boosts are DISABLED BY DESIGN: the audio worker thread runs at the
// default scheduling class and priority. Unprivileged Linux users cannot raise
// thread priority anyway (RLIMIT_RTPRIO/RLIMIT_NICE default to 0), and the
// resulting EPERM warnings provided no actionable value for a desktop player.
//
// Effective paths (observable at startup via the returned description):
//   Linux   -> no-op (priority boost disabled by design).
//   Windows -> THREAD_PRIORITY_ABOVE_NORMAL.
//   Apple   -> intentional no-op (no SCHED_RR for user threads, per-thread
//              nice is unavailable; a process-wide setpriority would wrongly
//              boost the device thread as well).

#include <string>

namespace seriona::audio {

enum class ThreadPriorityOutcome {
  // SCHED_RR priority 1 engaged (Linux).
  Realtime,
  // SCHED_RR denied (EPERM); nice -5 fallback engaged (Linux).
  Nice,
  // THREAD_PRIORITY_ABOVE_NORMAL engaged (Windows).
  AboveNormal,
  // Platform without per-thread priority control, or boost disabled by
  // design (Apple; Linux). By design no-op.
  NoOp,
  // Every attempt failed; the thread keeps its default priority.
  Denied,
};

struct ThreadPriorityResult {
  ThreadPriorityOutcome outcome{ThreadPriorityOutcome::Denied};
  std::string description;
};

#if defined(_WIN32)

#include <windows.h>

inline ThreadPriorityResult applyAudioWorkerThreadPriority() {
  if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL)) {
    return {ThreadPriorityOutcome::AboveNormal,
            "windows: worker thread priority set to THREAD_PRIORITY_ABOVE_NORMAL"};
  }
  return {ThreadPriorityOutcome::Denied,
          "windows: SetThreadPriority(THREAD_PRIORITY_ABOVE_NORMAL) failed; "
          "worker thread keeps its default priority"};
}

#elif defined(__APPLE__)

inline ThreadPriorityResult applyAudioWorkerThreadPriority() {
  return {ThreadPriorityOutcome::NoOp,
          "macos: no-op - no SCHED_RR for user threads and per-thread nice is "
          "unavailable; process-wide priority change deliberately avoided "
          "(it would also affect the audio device thread)"};
}

#else  // Linux and other POSIX platforms.

inline ThreadPriorityResult applyAudioWorkerThreadPriority() {
  return {ThreadPriorityOutcome::NoOp,
          "linux: priority boost disabled by design; worker thread runs at "
          "default priority"};
}

#endif

}  // namespace seriona::audio
