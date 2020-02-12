#ifndef ERROR_H
#define ERROR_H

#include <assert.h>

// Flag which determines whether to fail if a required condition is not met, or
// to adapt the condition in order to work properly.
// Attention: Be sure to perform a clean build after you alter preprocessor
//            directives / definitions.
//#define FAIL_ON_ERROR

#include <stdio.h>

///
/// Function used in case the fail-on-error mode is disabled (via definition)
/// to report errors. In debug production mode, alias st to st 2> error.log.
static void reportError(char const * cond, char const * stt, char const * file,
		unsigned int line ) {
	unsigned int const maxErrorCount = 100;
	static unsigned int errorCount = 0;
	if (++errorCount == 1) {
		printf("Report the following bug to "
				"https://github.com/juliusHuelsmann/st.\n");
	}
	if (errorCount < maxErrorCount) {
		printf("Bug:\tCondition '%s' evaluates to false.\n\tPerforming"
				" '%s' to counteract.\n\tFile:%s:%u\n",
				cond, stt, file, line);
	} else if (errorCount == maxErrorCount) {
		printf("Max amount of reported errors %u is reached. From here"
				"on, no additional errors will be reported.\n",
				maxErrorCount);
	}
}

/// Note that everyting condition checked / endforced with #ENSURE is
/// considered an error, and behaves like an error depending on the flag.
#ifdef FAIL_ON_ERROR
#define ENSURE(cond, stt) assert(cond);
#else // FAIL_ON_ERROR
#define ENSURE(cond, stt) if (!(cond)) {                                       \
                          	reportError(#cond, #stt, __FILE__, __LINE__);  \
                          	stt;                                           \
                          }
#endif // FAIL_ON_ERROR

#endif // ERROR_H
