#ifndef LOCKDECISION_H
#define LOCKDECISION_H

#include <QLockFile>
#include <QString>

/// Startup decision for a failed QLockFile::tryLock() attempt (C1, R15-001).
///
/// Pure message/exit-code mapping only -- QLockFile mechanics (creating the
/// lock, calling tryLock(), stale-lock recovery) stay in main()'s startup
/// composition. Kept separate so the "what does this failure mean to the
/// user" decision is a single small, readable place rather than interleaved
/// with lock-file plumbing.
namespace LockDecision {

/// Every tryLock() failure is fatal to startup (fail closed): the
/// application must not access the data directory without proven exclusive
/// ownership. The two outcomes only change the message/exit code, never
/// whether startup continues.
enum class Outcome {
    AnotherInstance,   // QLockFile::LockFailedError: a live owner holds the lock.
    OwnershipUnknown,  // PermissionError / UnknownError: exclusivity could not be proven.
};

inline Outcome classify(QLockFile::LockError error)
{
    return error == QLockFile::LockFailedError
        ? Outcome::AnotherInstance
        : Outcome::OwnershipUnknown;
}

inline QString message(Outcome outcome)
{
    switch (outcome) {
    case Outcome::AnotherInstance:
        return "Another instance of µTimer is already running.\n\n"
               "Please close it before starting a new one.";
    case Outcome::OwnershipUnknown:
        return "Could not establish exclusive ownership for µTimer "
               "(the instance lock file could not be created or locked).\n\n"
               "Please check that the application directory is writable and restart.";
    }
    return QString();
}

inline int exitCode(Outcome outcome)
{
    return outcome == Outcome::AnotherInstance ? 2 : 3;
}

} // namespace LockDecision

#endif // LOCKDECISION_H
