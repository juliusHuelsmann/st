/* See LICENSE for license details. */
#ifndef NORMAL_MODE_H
#define NORMAL_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Used in the configuration file to define custom shortcuts.
typedef struct NormalModeShortcuts {
	char key;
	char *value;
} NormalModeShortcuts;

/// Holds the exit status of the #kpressNormalMode function, which informs the
/// caller when to exit normal mode.
typedef enum ExitState {
	failed = 0,
	success = 1,
	finished = 2,
} ExitState;

/// Called when curr position is altered.
void onMove(void);

/// Function which returns whether the value at position provided as arguments
/// is to be highlighted.
int highlighted(int, int);

/// Handles keys in normal mode.
ExitState kpressNormalMode(char const * decoded, int len, bool ctrlPressed,
		void const * ksym);
		//bool esc, bool enter, bool backspace, void* keysym);


#endif // NORMAL_MODE_H
