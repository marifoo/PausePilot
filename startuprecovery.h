#ifndef STARTUPRECOVERY_H
#define STARTUPRECOVERY_H

class TimeTracker;

/**
 * Runs startup checkpoint recovery to completion BEFORE the main UI exists
 * (B2, section 2 "Resolved product and domain policies"). No tracking or
 * degraded/read-only mode is available before this returns true.
 *
 * Calls TimeTracker::initializeFromStore() once. On Success/Disabled,
 * returns true immediately so the caller can proceed to construct the main
 * window. On TransientError, shows a modal Retry/Quit choice:
 * each Retry repeats the whole atomic recovery via another
 * initializeFromStore() call; Quit returns false without asking again,
 * leaving existing rows/marker untouched for a later launch to retry.
 *
 * @return true if startup should continue (recovery succeeded or history is
 *         disabled); false if the user chose Quit -- the caller must exit
 *         without creating the main window.
 */
bool runStartupRecovery(TimeTracker& tracker);

#endif // STARTUPRECOVERY_H
