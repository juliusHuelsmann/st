/* See LICENSE for license details. */
#include "normalMode.h"
#include "dynamicArray.h"
#include "term.h"
#include "win.h"
#include "error.h"

#include <X11/keysym.h>
#include <X11/XKBlib.h>

#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

#define LEN(a)                 (sizeof(a) / sizeof(a)[0])
#define BETWEEN(x, a, b)       ((a) <= (x) && (x) <= (b))
//#define FALLTHROUGH            __attribute__((fallthrough));
#define FALLTHROUGH
#define SEC(var,ini,h,r)       var = ini; if (!var) { h; return r; }
#define EXPAND(v1,v2,r)        char *SEC(v1, expand(v2), empty(v2), true)
#define currentCommand         (toggle ? &commandHist0 : &commandHist1)
#define lastCommand            (toggle ? &commandHist1 : &commandHist0)

//
// Interface to the terminal
extern Glyph const styleCommand, styleSearch;
extern NormalModeShortcuts normalModeShortcuts[];
extern size_t const amountNormalModeShortcuts;
extern char wordDelimSmall[];
extern char wordDelimLarge[];
extern unsigned int fgCommandYank, fgCommandVisual, fgCommandVisualLine,
       bgCommandYank, bgCommandVisual, bgCommandVisualLine, bgPos, fgPos;

extern void selclear(void);
extern void tsetdirt(int, int);
extern size_t utf8encode(Rune, char *);
extern size_t utf8decode(const char *, Rune *, size_t);
extern size_t utf8decodebyte(char c, size_t *i);

extern void selextend(int, int, int, int, int);
extern void selstart(int, int, int, int);
extern char *getsel(void);
extern void tfulldirt(void);

//
// `Private` structs
typedef struct { uint32_t x; uint32_t y; uint32_t yScr; } Position;

/// Entire normal mode state, consisting of an operation and a motion.
typedef struct {
	Position initialPosition;
	struct OperationState {
		enum Operation {
			noop = ' ', visual='v', visualLine='V', yank = 'y' } op;
		Position startPosition;
		enum Infix { infix_none = 0, infix_i = 1, infix_a = 2, } infix;
	} command;
	struct MotionState {
		uint32_t amount;
		enum Search {none, forward, backward} search;
		Position searchPosition;
		bool finished;
	} motion;
} NormalModeState;

/// Default state if no operation is performed.
NormalModeState defaultNormalMode = {
	{0,0,0},    {noop, {0, 0, 0}, false},   {0, none, {0, 0, 0}, true}
};
NormalModeState stateVB = {
	{0,0,0},    {noop, {0, 0, 0}, false},   {0, none, {0, 0, 0}, true}
};

DynamicArray searchString =  UTF8_ARRAY;
DynamicArray commandHist0 =  UTF8_ARRAY;
DynamicArray commandHist1 =  UTF8_ARRAY;
DynamicArray highlights   = DWORD_ARRAY;

/// History command toggle
static bool toggle = false;

//
// Utility functions
static inline int intervalDiff(int v, int a, int b) {
	return (v < a) ? (v - a) : ((v > b) ? (v - b) : 0);
}
static inline void swap(DynamicArray *const a, DynamicArray *const b) {
	DynamicArray tmp = *a; *a = *b; *b = tmp;
}
static inline int max(int a, int b) { return a > b ? a : b; }
static inline int min(int a, int b) { return a < b ? a : b; }
static inline int mod(int a, int b) { for (; a < 0; a += b); return a % b; }
static inline bool contains (char c, char const * values, uint32_t memSize) {
	ENSURE(values != NULL, return false);
	for (uint32_t i = 0; i < memSize; ++i) if (c == values[i]) return true;
	return false;
}
static inline void applyPosition(Position const *pos) {
	ENSURE(pos != NULL, return);
	term.c.x = pos->x;
	term.c.y = pos->y;
	term.scr = pos->yScr;
}
static inline int getSearchDirection(void) {
	return stateVB.motion.search == forward ? 1 : -1;
}

//  Utilities for working with the current version of the scrollback patch.
static bool moveLine(int32_t const amount) {
	int32_t const reqShift = intervalDiff(term.c.y+=amount, 0, term.row-1);
	term.c.y -= reqShift;
	int32_t const sDiff = intervalDiff(term.scr-=reqShift, 0, HISTSIZE-1);
	term.scr -= sDiff;
	return sDiff == 0;
}

static void moveLetter(int32_t const amount) {
	int32_t value = (term.c.x += amount) / term.col;
	if (value -= (term.c.x < 0)) {
		term.c.x = moveLine(value) ? mod(term.c.x, term.col)
			: max(min(term.c.x,term.col - 1), 0);
	}
	assert(BETWEEN(term.c.x,0,term.col-1)&&BETWEEN(term.c.y,0,term.row-1));
}

//
// `Private` functions:

// Functions: Temporarily display string on screen.

/// Display string at end of a specified line without writing it into the buffer
/// @param str  string that is to be displayed
/// @param g    glyph
/// @param yPos
static void
displayString(DynamicArray const *str, Glyph const *g, int yPos, bool prePos) {
	ENSURE((str != NULL) && (g != NULL) && (term.row > 0), return);
	ENSURE(yPos >= 0, yPos = 0);
	ENSURE(yPos < term.row, yPos = term.row - 1);
	// Arbritary limit to avoid withhelding too much info from user.
	int const maxFractionOverridden = 3;
	// Threshold: if there is no space to print, do not print, but transfer
	//            repsonsibility for printing back to [st].
	if (term.col < maxFractionOverridden) {                        // (0)
		term.dirty[yPos] = 1;
		return;
	}
	int32_t const botSz = prePos * 6; //< sz for position indication
	// Determine the dimensions of used chunk of screen.
	int32_t const overrideSize = min(size(str) + botSz,
			term.col / maxFractionOverridden);             // (1)
	int32_t const overrideEnd = term.col - 2;
	// Has to follow trivially hence th assert:
	// overrideSize <(1)= term.col/3  <(0)= term.col = overrideEnd + 1.
	assert(overrideSize <= overrideEnd + 1);
	int32_t const overrideStart = 1 + overrideEnd - overrideSize;
	// display history[history.size() - (overrideSize - botSz)::-1]
	Glyph *SEC(line, malloc(sizeof(Glyph) * (overrideSize)),,)
	int32_t offset = (size(str) - overrideSize - 1 + botSz) * str->itemSize;
	for (uint32_t chr = 0; chr < overrideSize - botSz; ++chr) {
		line[chr] = *g;
		line[chr].u = *((Rune*) (str->content+(offset+=str->itemSize)));
	}
	if (prePos) {
		ENSURE(term.scr < HISTSIZE, term.scr = HISTSIZE - 1);
		int const p=(int)(0.5+(HISTSIZE-1-term.scr)*100./(HISTSIZE-1));
		int const v = min(max(p, 0), 100);
		char prc [10];
		switch (term.scr) {
			case HISTSIZE - 1: strcpy(prc, " [TOP]"); break;
			case 0:            strcpy(prc, " [BOT]"); break;
			default:           sprintf(prc, " % 3d%c  ", v, '%');
		}
		for (uint32_t chr = 0; chr < botSz; ++chr) {
			line[chr + overrideSize - botSz] =*g;
			line[chr + overrideSize - botSz].fg = fgPos;
			line[chr + overrideSize - botSz].bg = bgPos;
			utf8decode(&prc[chr],&line[chr+overrideSize-botSz].u,1);
		}
		line[overrideSize - botSz] =*g;
	}
	xdrawline(TLINE(yPos), 0, yPos, overrideStart);
	term.c.y -= term.row; term.c.x -= term.col; // not highlight hack
	xdrawline(line-overrideStart, overrideStart, yPos, overrideEnd + 1);
	term.c.y += term.row; term.c.x += term.col;
	free(line);
}

static inline void printCommandString(void) {
	Glyph g = styleCommand;
	switch(stateVB.command.op) {
		case yank: g.fg = fgCommandYank; g.bg = bgCommandYank; break;
		case visual: g.fg=fgCommandVisual; g.bg=bgCommandVisual; break;
		case visualLine: g.fg=fgCommandVisualLine;
				 g.bg=bgCommandVisualLine;
	}
	displayString(isEmpty(currentCommand) ? lastCommand : currentCommand,
			&g, term.row - 1, true);
}

static inline void printSearchString(void) {
	displayString(&searchString, &styleSearch, term.row - 2, false);
}

// NormalMode Operation / Motion utilies.

static inline bool isMotionFinished(void) { return stateVB.motion.finished; }

static inline void finishMotion(void) { stateVB.motion.finished = true; }

static inline bool isOperationFinished(void) {
	return stateVB.command.op==noop && stateVB.command.infix==infix_none;
}

/// Register that the current comamnd is finished and a new command is lgoged
static inline  void startNewCommand(bool abort) {
	if (!abort) { toggle = !toggle; }
	empty(currentCommand);
}

static inline void finishOperation(void) {
	stateVB.command = defaultNormalMode.command;
	assert(isOperationFinished());
	// After an operation is finished, the selection has to be released and
	// no highlights are to be released.
	selclear();
	empty(&highlights);
	// THe command string is reset for a new command.
	startNewCommand(true);
}

static inline void enableOperation(enum Operation o) {
	finishOperation();
	stateVB.command.op = o;
	stateVB.command.infix = infix_none;
	stateVB.command.startPosition.x = term.c.x;
	stateVB.command.startPosition.y = term.c.y;
	stateVB.command.startPosition.yScr = term.scr;
}

/// @param abort: If enabled, the command exits without registering
/// @return       Whether the the application is ready to yield control back to
//the normal command flow
static bool terminateCommand(bool abort) {
	bool const exitOperation = isMotionFinished();
	bool exitNormalMode = false;
	finishMotion();

	if (exitOperation) {
		exitNormalMode = isOperationFinished();
		finishOperation();
	}
	printCommandString();
	printSearchString();
	return exitNormalMode;
}

static inline void exitCommand(void) { terminateCommand(false); }

static inline void abortCommand(void) { terminateCommand(true); }

/// Go to next occurrence of string relative to the current location
/// conduct search, starting at start pos
static bool gotoString(int8_t sign) {
	moveLetter(sign);
	uint32_t const searchStrSize = size(&searchString);
	uint32_t const maxIter = (HISTSIZE+term.row) * term.col + searchStrSize;
	uint32_t findIdx = 0;
	for (uint32_t cIteration = 0; findIdx < searchStrSize
			&& ++cIteration <= maxIter; moveLetter(sign)) {
		char const * const SEC(next, sign==1
				? view(&searchString, findIdx)
				: viewEnd(&searchString, findIdx), , false)
		uint32_t const searchChar = *((uint32_t*) next);

		if (TLINE(term.c.y)[term.c.x].u == searchChar) { ++findIdx; }
		else { findIdx = 0; }
	}
	bool const found = findIdx == searchStrSize;
	for (uint32_t i = 0; found && i < searchStrSize; ++i) moveLetter(-sign);
	return found;
}

/// Highlight all found strings on the current screen.
static void highlightStringOnScreen(void) {
	if (isEmpty(&searchString)) { return; }
	empty(&highlights);
	uint32_t const searchStringSize = size(&searchString);
	uint32_t findIdx = 0;
	uint32_t xStart, yStart;
	bool success = true;
	for (int y = 0; y < term.row && success; y++) {
		for (int x = 0; x < term.col && success; x++) {
			char const* const SEC(next,
					view(&searchString,findIdx),,)
			if (TLINE(y)[x].u == (Rune) *((uint32_t*)(next))) {
				if (++findIdx == 1) {
					xStart = x;
					yStart = y;
				}
				if (findIdx == searchStringSize) {
					success = success
						&& append(&highlights, &xStart)
						&& append(&highlights, &yStart);
					findIdx = 0; //term.dirty[yStart] = 1;
				}
			} else { findIdx = 0; }
		}
	}
	if (!success) { empty(&highlights); }
}

static bool gotoStringAndHighlight(int8_t sign) {
      	// Find hte next occurrence of the #searchString in direction #sign
	bool const found = gotoString(sign);
	if (!found) {  applyPosition(&stateVB.motion.searchPosition); }
	highlightStringOnScreen();
	//tsetdirt(0, term.row-3); //< everything except for the 'status bar'
	return found;
}

static bool pressKeys(char const* nullTerminatedString, size_t end) {
        bool sc = true;
	for (size_t i = 0; i < end && sc; ++i) {
		sc = kpressNormalMode(&nullTerminatedString[i], 1, false, NULL);
	}
	return sc;
}

static bool executeCommand(DynamicArray const *command) {
	size_t end=size(command);
	char decoded [32];
	bool succ = true;
	size_t len;
	for (size_t i = 0; i < end && succ; ++i) {
		char const *const SEC(nextRune, view(command, i),,false)
		len = utf8encode(*((Rune *) nextRune), decoded);
		succ = kpressNormalMode(decoded, len, false, NULL);
	}
	return succ;
}

struct { char const first; char const second; } const Brackets [] =
{ {'(', ')'}, {'<', '>'}, {'{', '}'}, {'[', ']'}, };


/// Emits Command prefix and suffix when i motion is performed (e.g. yiw).
///
/// @param c:             motion character
/// @param expandMode:    1 for 'i', 2 for 'a'
/// @param first, second: Dynamic arrays in which the prefix and postfix
///                       commands will be returned
/// @return               whether the command could be extracted successfully.
static bool expandExpression(char const c, enum Infix expandMode,
		char operation, DynamicArray *cmd) {
	empty(cmd);
	bool s = true; //< used in order to detect memory allocation errors.
	char const lower = tolower(c);
	// Motions
	if (lower == 'w') {
		// translated into wb[command]e resp. WB[command]E, which works
		// file even when at the fist letter. Does not work for single
		// letter words though.
		int const diff = c - lower;
		s = s && checkSetNextV(cmd, c);
		s = s && checkSetNextV(cmd, (signed char)(((int)'b') + diff));
		s = s && checkSetNextV(cmd, operation);
		s = s && checkSetNextV(cmd, (signed char)(((int)'e')+ diff));
		return s;
	}
	// Symmetrical brackets (quotation marks)
	if (c == '\'' || c == '"') {
		// Local ambiguity -> do nothing. It cannot be determined if
		// the current char is the 1st or last char of the selection.
		//  <---- search here? -- ['] -- or search here? --->
		if (TLINE(term.c.y)[term.c.x].u == c) {
			return false;
		}
		// Prefix
		char res [] = {'?', c, '\n'};
		s = s && checkSetNextP(cmd, res);
		// infix
		bool const iffy = expandMode == infix_i;
		if (iffy) { s = s && checkSetNextV(cmd, 'l'); }
		s = s && checkSetNextV(cmd, operation);
		if (!iffy) { s = s && checkSetNextV(cmd, 'l'); }
		// suffix
		res[0] = '/';
		s = s && checkSetNextP(cmd, res);
		if (iffy) { s = s && checkSetNextV(cmd, 'h'); }
		return s;
	}
	// Brackets: Does not if in range / if the brackets belong togehter.
	for (size_t pid = 0; pid < sizeof(Brackets); ++pid) {
		if(Brackets[pid].first == c || Brackets[pid].second == c) {
			if (TLINE(term.c.y)[term.c.x].u!=Brackets[pid].first) {
				s = s && checkSetNextV(cmd, '?');
				s = s && checkSetNextV(cmd, Brackets[pid].first);
				s = s && checkSetNextV(cmd, '\n');
			}
			bool const iffy = expandMode == infix_i;
			if (iffy) { s = s && checkSetNextV(cmd, 'l'); }
			s = s && checkSetNextV(cmd, operation);
			if (!iffy) { s = s && checkSetNextV(cmd, 'l'); }
			s = s && checkSetNextV(cmd, '/');
			s = s && checkSetNextV(cmd, Brackets[pid].second);
			s = s && checkSetNextV(cmd, '\n');
			if (iffy) { s = s && checkSetNextV(cmd, 'h'); }
			return s;
		}
	}
	/**/
	// search string
	// complicated search operation: <tag>
	if (c == 't') {
		// XXX: (Bug in vim: @vit )
		// <tag_name attr="hier" a2="\<sch\>"> [current pos] </tag_name>

		// 1. Copy history ( tag := hist[?<\n:/ \n] )
		// 2. Copy history ( first_find := hist[?<\n: next place in
		//                   history where count '>' > count '<'
		//                   (can be behind current pos) )
		// 3. first := [?first_find][#first_ind]l
		//    second:= [/tag">"]h
		//return true; // XXX: not implmented yet.
	}
	return false;
}

//
// Public API
//

void onMove(void) {
	stateVB.initialPosition.x = term.c.x;
	stateVB.initialPosition.y = term.c.y;
	stateVB.initialPosition.yScr = term.scr;
}

int highlighted(int x, int y) {
	// Compute the legal bounds for a hit:
	int32_t const stringSize = size(&searchString);
	int32_t xMin = x - stringSize;
	int32_t yMin = y;
	while (xMin < 0 && yMin > 0) {
		xMin += term.col;
		--yMin;
	}
	if (xMin < 0) { xMin = 0; }

	uint32_t highSize = size(&highlights);
	ENSURE(highSize % 2 == 0, empty(&highlights); return false;);
	highSize /= 2;
	uint32_t *ptr = (uint32_t*) highlights.content;
	for (uint32_t i = 0; i < highSize; ++i) {
		int32_t const sx = (int32_t) *(ptr++);
		int32_t const sy = (int32_t) *(ptr++);
		if (BETWEEN(sy, yMin, y) && (sy != yMin || sx > xMin)
				&& (sy != y || sx <= x)) {
			return true;
		}
	}
	return false;
}

ExitState kpressNormalMode(char const * cs, int len, bool ctrl, void const *v) {
	KeySym const * const ksym = (KeySym*) v;
	bool const esc = ksym &&  *ksym == XK_Escape;
	bool const enter = (ksym && *ksym==XK_Return) || (len==1 &&cs[0]=='\n');
	bool const quantifier = len == 1 && (BETWEEN(cs[0], 49, 57)
			|| (cs[0] == 48 && stateVB.motion.amount));
	int const previousScroll = term.scr;
	// [ESC] or [ENTER] abort resp. finish the current level of operation.
	// Typing 'i' if no operation is currently performed behaves like ESC.
	if (esc || enter || (len == 1 && cs[0] == 'i' && isMotionFinished()
				&& isOperationFinished())) {
		if (terminateCommand(!enter)) {
			applyPosition(&stateVB.initialPosition);
			Position const pc = stateVB.initialPosition;
			stateVB = defaultNormalMode;
			stateVB.initialPosition = pc;
			tfulldirt();
			return finished;
		}
		len = 0;
		goto motionFinish;
	}
	// Backspace
	if (ksym && *ksym == XK_BackSpace) {
		bool s = stateVB.motion.search!=none&&!stateVB.motion.finished;
		bool q = stateVB.motion.amount != 0;
		if (!(s || q)) { return failed; }
		len = 0;

		if (!isEmpty(currentCommand)) { pop(currentCommand); }
		if (s) {
			if (!isEmpty(&searchString)) { pop(&searchString); }
			else if (isEmpty(&searchString)) {
				exitCommand();
				return success;
			}
		} else if (q) {
			stateVB.motion.amount /= 10;
			goto finishNoAppend;
		}
	}

	// Search: append to search string, then search & highlight
	if (stateVB.motion.search != none && !stateVB.motion.finished) {
		if (len >= 1) {
			EXPAND(kSearch, &searchString, true)
			utf8decode(cs, (Rune*)(kSearch), len);
		}
		applyPosition(&stateVB.motion.searchPosition);
		gotoStringAndHighlight(getSearchDirection());
		goto finish;
	}
	if (len == 0) { return failed; }
	// Quantifiers
	if (quantifier) {
		stateVB.motion.amount = min(SHRT_MAX,
				stateVB.motion.amount * 10 + cs[0] - 48);
		goto finish;
	}
	// 'i' mode enabled, hence the expression is to be expanded:
	// [start_expression(cs[0])] [operation] [stop_expression(cs[0])]
	if (stateVB.command.infix != infix_none && stateVB.command.op != noop) {
		DynamicArray cmd = CHAR_ARRAY;
		char const operation = stateVB.command.op;
		bool succ = expandExpression(cs[0],
				stateVB.command.infix, visual, &cmd);
		if (operation == yank) {
			succ = succ && checkSetNextV(&cmd, operation);
		}
		NormalModeState const st = stateVB;
		TCursor         const tc = term.c;
		stateVB.command.infix    = infix_none;
		if (succ) {
			stateVB.command.op = noop;
			for (int i = 0; i < size(&cmd) && succ; ++i) {
				succ = pressKeys(&cmd.content[i], 1);
			}
			if (!succ) { // go back to the old position, apply op
				stateVB = st;
				term.c = tc;
			}
			empty(currentCommand);
			for (uint32_t i = 0; i < size(&cmd); ++i) {
				EXPAND(kCommand, currentCommand, true)
				utf8decode(cmd.content+i, (Rune*)(kCommand),1);
			}
		}
		free(cmd.content);
		goto finishNoAppend;
	}
	// Commands (V / v or y)
	switch(cs[0]) {
		case '.':
		{
			if (isEmpty(currentCommand)) { toggle = !toggle; }
			DynamicArray cmd = UTF8_ARRAY;
			swap(&cmd, currentCommand);
			executeCommand(&cmd) ? success : failed;
			swap(&cmd, currentCommand);
			free(cmd.content);
			goto finishNoAppend;
		}
		case 'i': stateVB.command.infix = infix_i; goto finish;
		case 'a': stateVB.command.infix = infix_a; goto finish;
		case 'y':
			switch(stateVB.command.op) {
				case noop: //< Start yank mode & set #op
					enableOperation(yank);
					selstart(term.c.x, term.c.y,term.scr,0);
					goto finish;
				case yank: //< Complete yank [y#amount j]
					selstart(0, term.c.y, term.scr, 0);
					int const origY = term.c.y;
					moveLine(max(stateVB.motion.amount, 1));
					selextend(term.col-1,term.c.y,term.scr,
							SEL_RECTANGULAR, 0);
					term.c.y = origY;
					FALLTHROUGH
				case visualLine: // Yank visual selection
				case visual:
					xsetsel(getsel());
					xclipcopy();
					exitCommand();
					goto finish;
				default:
					return failed;
			}
		case visual:
		case visualLine:
			if (stateVB.command.op == cs[0]) {
				finishOperation();
				return true;
			} else {
				enableOperation(cs[0]);
				selstart(cs[0] == visualLine ? 0 : term.c.x,
						term.c.y, term.scr, 0);
				goto finish;
			}
	}
	// CTRL Motions
	int32_t sign = -1;    //< if command goes 'forward'(1) or 'backward'(-1)
	if (ctrl) {
		if (ksym == NULL) { return false; }
		switch(*ksym) {
			case XK_f:
				term.scr = max(term.scr - max(term.row-2,1), 0);
				term.c.y = 0;
				goto finish;
			case XK_b:
				term.scr = min(term.scr + max(term.row - 2, 1),
						HISTSIZE - 1);
				term.c.y = term.bot;
				goto finish;
			case XK_u:
				term.scr = min(term.scr+term.row/2, HISTSIZE-1);
				goto finish;
			case XK_d:
				term.scr = max(term.scr - term.row / 2, 0);
				goto finish;
			default: return false;
		}
	}
	// Motions
	switch(cs[0]) {
		case 'c': empty(&commandHist0); empty(&commandHist1);
			  goto finishNoAppend;
		case 'j': sign = 1; FALLTHROUGH
		case 'k': moveLine(max(stateVB.motion.amount,1) * sign);
			  goto motionFinish;
		case 'H': term.c.y = 0;
			  goto motionFinish;
		case 'M': term.c.y = term.bot / 2;
			  goto motionFinish;
		case 'L': term.c.y = term.bot;
			  goto motionFinish;
		case 'G': applyPosition(&stateVB.initialPosition);
			  goto motionFinish;
		case 'l': sign = 1; FALLTHROUGH
		case 'h': moveLetter(sign * max(stateVB.motion.amount,1));
			  goto motionFinish;
		case '0': term.c.x = 0;
			  goto motionFinish;
		case '$': term.c.x = term.col-1;
			  goto motionFinish;
		case 'w': FALLTHROUGH
		case 'W': FALLTHROUGH
		case 'e': FALLTHROUGH
		case 'E': sign = 1; FALLTHROUGH
		case 'B': FALLTHROUGH
		case 'b': {
			char const * const wDelim =
				cs[0] <= 90 ? wordDelimLarge : wordDelimSmall;
			uint32_t const wDelimLen = strlen(wDelim);

			bool const startSpaceIsSeparator =
				!(cs[0] == 'w' || cs[0] == 'W');
			// Whether to start & end with offset:
			bool const performOffset = startSpaceIsSeparator;
			// Max iteration := One complete hist traversal.
			uint32_t const maxIter = (HISTSIZE+term.row) * term.col;
			// Doesn't work exactly as in vim: Linebreak is
			// counted as 'normal' separator, hence a jump can
			// span multiple lines here.
			stateVB.motion.amount = max(stateVB.motion.amount, 1);
			for (;stateVB.motion.amount>0;--stateVB.motion.amount) {
				uint8_t state = 0;
				if (performOffset) { moveLetter(sign); }
				for (uint32_t cIt = 0; cIt ++ < maxIter; moveLetter(sign)) {
					if (startSpaceIsSeparator == contains(TLINE(term.c.y)[term.c.x].u, wDelim, wDelimLen)) {
						if (state == 1) {
							if (performOffset) {
								moveLetter(-sign);
							}
							break;
						}
					} else if (state == 0) { state = 1; }
				}
			}
			goto motionFinish;
		}
		case '/': sign = 1; FALLTHROUGH
		case '?':
			  empty(&searchString);
			  stateVB.motion.search = sign == 1 ? forward : backward;
			  stateVB.motion.searchPosition.x = term.c.x;
			  stateVB.motion.searchPosition.y = term.c.y;
			  stateVB.motion.searchPosition.yScr = term.scr;
			  stateVB.motion.finished = false;
			  goto finish;
		case 'n': sign = 1; FALLTHROUGH
		case 'N': {
			if (stateVB.motion.search == none) return failed;
			if (stateVB.motion.search == backward) { sign *= -1; }
			bool b = true; int ox = term.c.x;
			int oy = term.c.y ; int scr = term.scr;
			int32_t i = max(stateVB.motion.amount, 1);
			for (;i>0 && (b=gotoString(sign)); --i) {
                          oy = term.c.y; scr = term.scr;
			}
			if (!b) { term.c.x = ox; term.c.y = oy; term.scr = scr;}
			goto motionFinish;
		}
		case 't': // Toggle selection mode and set dirt.
			  sel.type = sel.type == SEL_REGULAR
				  ? SEL_RECTANGULAR : SEL_REGULAR;
			  //tsetdirt(sel.nb.y, sel.ne.y);
			  goto motionFinish;
	}
	// Custom commands
	for (size_t i = 0; i < amountNormalModeShortcuts; ++i) {
		if (cs[0] == normalModeShortcuts[i].key) {
			return pressKeys(normalModeShortcuts[i].value,
					strlen(normalModeShortcuts[i].value))
					? success : failed;
		}
	}
	return failed;
motionFinish:
	stateVB.motion.amount = 0;
	//if (isMotionFinished() && stateVB.command.op == yank) {
	if (stateVB.command.op == yank) {
		selextend(term.c.x, term.c.y, term.scr, sel.type, 0);
		xsetsel(getsel());
		xclipcopy();
		exitCommand();
	}
finish:
	if (len == 1 && !ctrl) { // XXX: for now.
		EXPAND(kCommand, currentCommand, true)
		utf8decode(cs, (Rune*)(kCommand), len);
	}
finishNoAppend:
	if (stateVB.command.op == visual) {
		selextend(term.c.x, term.c.y, term.scr, sel.type, 0);
	} else if (stateVB.command.op == visualLine) {
		selextend(term.col-1, term.c.y, term.scr, sel.type, 0);
	}

	if (previousScroll != term.scr && !isEmpty(&searchString)) {
		highlightStringOnScreen();
	}
	tsetdirt(0, term.row-3); //< Required because of the cursor cross.
	printCommandString();
	printSearchString();
	return success;
}
