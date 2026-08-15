#pragma once

// Per-thread scheduling priority helper for the audio worker thread.
//
// The worker thread runs the command/decode loop and must be scheduled above
// the default interactive priority so PCM queue fill stays ahead of the
// miniaudio device thread (RT callback path in audio_output_device.cpp is
// untouched). This helper operates ONLY on the calling thread; it never
// changes process-wide priority, so the device callback thread and every
// other thread in the process keep their scheduling class.
//
// Effective paths (observable at startup via the returned description):
//   Linux   -> SCHED_RR priority 1; on EPERM falls back to nice -5
//              (setpriority); if both fail the thread keeps default priority.
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
  // Platform without per-thread priority control (Apple); by design no-op.
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

#include <cerrno>

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

inline ThreadPriorityResult applyAudioWorkerThreadPriority() {
  sched_param param{};
  param.sched_priority = 1;
  const int rtResult = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
  if (rtResult == 0) {
    return {ThreadPriorityOutcome::Realtime,
            "linux: worker thread set to SCHED_RR priority 1"};
  }

  if (rtResult == EPERM) {
    if (setpriority(PRIO_PROCESS, 0, -5) == 0) {
      return {ThreadPriorityOutcome::Nice,
              "linux: SCHED_RR denied (EPERM); worker thread set to nice -5"};
    }
    return {ThreadPriorityOutcome::Denied,
            "linux: SCHED_RR priority 1 denied (EPERM) and nice -5 denied "
            "(EPERM); worker thread keeps default priority. RLIMIT_RTPRIO and "
            "RLIMIT_NICE default to 0, which forbids unprivileged priority "
            "boosts; add '- rtprio 1' (and optionally '- nice -5') for this "
            "user in /etc/security/limits.conf and re-login to allow SCHED_RR "
            "priority 1"};
  }

  return {ThreadPriorityOutcome::Denied,
          std::string("linux: pthread_setschedparam(SCHED_RR, priority 1) "
                      "failed with error code ") +
              std::to_string(rtResult) +
              "; worker thread keeps its default priority"};
}

#endif

}  // namespace seriona::audio
