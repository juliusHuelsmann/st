/* See LICENSE for license details. */
#include "normalMode.h"
#include "dynamicArray.h"
#include "term.h"
#include "win.h"

#include <ctype.h>
#include <stdio.h>
#include <limits.h>
#include <math.h>

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])
#define BETWEEN(x, a, b)	((a) <= (x) && (x) <= (b))
#define FALLTHROUGH		__attribute__((fallthrough));

// Config
extern Glyph const styleCommand;
extern Glyph const styleSearch;
extern NormalModeShortcuts normalModeShortcuts[];
extern size_t const amountNormalModeShortcuts;
extern char wordDelimSmall[];
extern char wordDelimLarge[];

extern void selclear(void);
extern void tsetdirt(int, int);
extern size_t utf8encode(Rune, char *);
extern size_t utf8decode(const char *, Rune *, size_t);

extern void selextend(int, int, int, int, int);
extern void selstart(int, int, int, int);
extern char *getsel(void);
extern void tfulldirt(void);


/// Position (x, y , and current scroll in the y dimension).
typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t yScr;
} Position;

static void applyPosition(Position const *pos) {
	term.c.x = pos->x;
	term.c.y = pos->y;
	term.scr = pos->yScr;
}



/// The entire normal mode state, consisting of an operation
/// and a motion.
typedef struct {
	Position initialPosition;
	// Operation:
	struct OperationState {
		enum Operation {
			noop = ' ',
			visual = 'v',
			visualLine = 'V',
			yank = 'y'
		} op;
		Position startPosition;
		enum Infix {
			infix_none = 0,
			infix_i = 1,
			infix_a = 2,
		} infix;
	} command;
	// Motions:
	struct MotionState {
		uint32_t amount;
		enum Search {
			none,
			forward,
			backward,
		} search;
		Position searchPosition;
		bool finished;
	} motion;
} NormalModeState;

NormalModeState defaultNormalMode = {
	{0,0,0}, 
	{noop, {0, 0, 0}, false}, 
	{0, none, {0, 0, 0}, true}
};

/// Default state if no operation is performed.
NormalModeState stateNormalMode = {
	{0,0,0}, 
	{noop, {0, 0, 0}, false}, 
	{0, none, {0, 0, 0}, true}
};

static inline int getSearchDirection(void) {
	return stateNormalMode.motion.search == forward ? 1 : -1;
}


DynamicArray searchString =  UTF8_ARRAY;
DynamicArray commandHist0 =  UTF8_ARRAY;
DynamicArray commandHist1 =  UTF8_ARRAY;
DynamicArray highlights   = QWORD_ARRAY;


/// History command toggle
bool toggle = false;
#define currentCommand toggle ? &commandHist0 : &commandHist1
#define lastCommand    toggle ? &commandHist1 : &commandHist0


int
highlighted(int x, int y)
{
	// Compute the legal bounds for a hit:
	int32_t const stringSize = size(&searchString);
	int32_t xMin = x - stringSize;
	int32_t yMin = y;
	while (xMin < 0 && yMin > 0) { //< I think this temds to be more efficient than
		xMin += term.col;            //  division + modulo.
		--yMin;
	}
	if (xMin < 0) { xMin = 0; }

	uint32_t highSize = size(&highlights);
	uint32_t *ptr = (uint32_t*) highlights.content;
	for (uint32_t i = 0; i < highSize; ++i) {
		int32_t const sx = *(ptr++);
		int32_t const sy = *(ptr++);
		if (BETWEEN(sy, yMin, y) && (sy != yMin || sx > xMin) && (sy != y || sx <= x)) {
			return true;
		}
	}
	return false;
}

static inline int mod(int a, int b) {
	for (;a < 0; a+=b);
	return a % b;
}

static void displayString(DynamicArray const *str, Glyph *g, int yPos) {
	// Threshold: if there is nothing or no space to print, do not print.
	if (term.col == 0 || str->index == 0) {
		term.dirty[yPos] = 1; //< mark this line as 'dirty', because the line is not
		//  marked dirty when scrolling due to string display.
		return;
	}

	uint32_t lineSize = MIN(size(str), term.col / 3);
	uint32_t xEnd = term.col - 1;
	assert(lineSize <= 1 + xEnd); //< as lineSize <= term.col/3 <= term.col - 1 + 1 = xEnd + 1
	uint32_t xStart = 1 + xEnd - lineSize;

	Line line = malloc(sizeof(Glyph) * lineSize);
	assert(str->index - 1 >=  lineSize - 1); //< lineSize <= str->index -1 direct premise.

	for (uint32_t lineIdx = 0; lineIdx < lineSize; lineIdx++) {
		line[lineIdx] = *g;
		char* end = viewEnd(str, lineSize - lineIdx - 1);
		memcpy(&line[lineIdx].u, end, str->itemSize);
	}
	xdrawline(TLINE(yPos), 0, yPos, xStart);
	xdrawline(line -xStart, xStart, yPos, xEnd+1);
	free(line); // that sucks.
}

/// Print either the current command or the last comman din case the current command is empty.
static inline void printCommandString(void) {
	Glyph g = styleCommand;
	displayString(isEmpty(currentCommand) ? lastCommand : currentCommand,
			&g, term.row - 1);
}

static inline void printSearchString(void) {
	Glyph g = styleSearch;
	displayString(&searchString, &g, term.row - 2);
}

static inline void enableOperation(enum Operation o) {
	stateNormalMode.command.op = o;
	stateNormalMode.command.infix = infix_none;
	stateNormalMode.command.startPosition.x = term.c.x;
	stateNormalMode.command.startPosition.y = term.c.y;
	stateNormalMode.command.startPosition.yScr = term.scr;
}

static inline bool isMotionFinished(void) {
	return stateNormalMode.motion.finished;
}

static inline void finishMotion(void) {
	stateNormalMode.motion.finished = true;
}

static inline bool isOperationFinished(void) {
	return stateNormalMode.command.op == noop
		&& stateNormalMode.command.infix == infix_none;
}

static inline void finishOperation(void) {
	stateNormalMode.command = defaultNormalMode.command;
	assert(isOperationFinished());
}

static bool replay = false;

static inline void emptyCurrentCommand(void) {
	if (!replay) { empty(currentCommand); }
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
		selclear();

		if (!replay) {
			emptyCurrentCommand();
			if (!abort) { toggle = !toggle; }
		}
		empty(&highlights);
	}
	printCommandString();
	printSearchString();
	return exitNormalMode;
}

static inline void exitCommand(void) { terminateCommand(false); }

static inline void abortCommand(void) { terminateCommand(true); }

static void moveLine(int8_t sign) {
	if (sign == -1) {
		if (term.c.y-- == 0) {
			if (++term.scr == HISTSIZE) {
				term.c.y = term.row - 1;
				term.scr = 0;
			} else {
				term.c.y = 0;
			}
		}
	} else {
		term.c.x = 0;
		if (++term.c.y == term.row) {
			if (term.scr-- == 0) {
				term.c.y = 0;
				term.scr = HISTSIZE - 1;
			} else {
				term.c.y = term.row - 1;
			}
		}
	}
}

static void moveLetter(int8_t sign) {
	term.c.x += sign;
	if (!BETWEEN(term.c.x, 0, term.col-1)) {
		if (term.c.x < 0) {
			term.c.x = term.col - 1;
			moveLine(sign);
		} else {
			term.c.x = 0;
			moveLine(sign);
		}
	}
}

static inline bool contains (char ksym, char const * values, uint32_t amount) {
	for (uint32_t i = 0; i < amount; i++) { if (ksym == values[i]) { return true; } }
	return false;
}


/// Go to next occurrence of string relative to the current location
/// conduct search, starting at start pos
bool
gotoString(int8_t sign) {
	uint32_t findIndex = 0;
	uint32_t searchStringSize = size(&searchString);
	uint32_t const maxIteration = (HISTSIZE + term.row) * term.col + searchStringSize;  //< one complete traversal.
	//moveLetter(sign); // XXX: new
	for (uint32_t cIteration = 0; findIndex < searchStringSize
			&& cIteration ++ < maxIteration; moveLetter(sign)) {
		uint32_t const searchChar = *((uint32_t*)(sign == 1 ? view(&searchString, findIndex)
					: viewEnd(&searchString, findIndex)));

		uint32_t const fu = TLINE(term.c.y)[term.c.x].u;

		if (fu == searchChar) findIndex++;
		else findIndex = 0;
	}
	bool const found = findIndex == searchStringSize;
	if (found) { for (uint32_t i = 0; i < searchStringSize; i++) { moveLetter(-sign); } }
	return found;
}

/// Find the next occurrence of a word
static inline bool
gotoNextString(int8_t sign) {
	moveLetter(sign);
	return gotoString(sign);
}

/// Highlight all found strings on the current screen.
static void
highlightStringOnScreen(void) {
	if (isEmpty(&searchString)) { return; }
	uint32_t const searchStringSize = size(&searchString);
	uint32_t findIndex = 0;
	uint32_t xStart, yStart;
	for (int y = 0; y < term.row; y++) {
		for (int x = 0; x < term.col; x++) {
			if (TLINE(y)[x].u == *((uint32_t*)(view(&searchString, findIndex)))) {
				if (findIndex++ == 0) {
					xStart = x;
					yStart = y;
				}
				if (findIndex == searchStringSize) {
					// mark selected
					append(&highlights, &xStart);
					append(&highlights, &yStart);

					findIndex = 0;
					term.dirty[yStart] = 1;
				}
			} else {
				findIndex = 0;
			}
		}
	}
}

static bool gotoStringAndHighlight(int8_t sign) {
	bool const found = gotoString(sign);  //< find the next string to the current position
	empty(&highlights);             //< remove previous highlights
	if (found) {                          //< apply new highlights if found
		//if (sign == -1) { moveLetter(-1); }
		highlightStringOnScreen();
	} else {                              //< go to the position where the search started.
		applyPosition(&stateNormalMode.motion.searchPosition);
	}
	tsetdirt(0, term.row-3);              //< repaint everything except for the status bar, which
	                                      //  is painted separately.
	return found;
}

static bool pressKeys(char const* nullTerminatedString, size_t end) {
	replay = true;
        bool succ = true;
	for (size_t i = 0; i < end && succ; ++i) {
		succ = kpressNormalMode(&nullTerminatedString[i], 1, false,
				false, nullTerminatedString[i] == '\n', false);
	}
	replay = false;
	return succ;
}

static bool executeCommand(DynamicArray const *command) {
	replay = true;
	size_t end=size(command);
	char decoded [32];
	bool succ = true;
	size_t len;
	for (size_t i = 0; i < end && succ; ++i) {
		len = utf8encode(*((Rune*)view(command, i)), decoded);
		succ = kpressNormalMode(decoded, len, false,
				false, len == 1 && decoded[0]=='\n', false);
	}
	replay = false;
	return succ;
}

struct {
	char const first;
	char const second;
} const Brackets [] = {
	{'(', ')'},
	{'<', '>'},
	{'{', '}'},
	{'[', ']'},
};


/// Emits Command prefix and suffix when i motion is performed (e.g. yiw).
///
/// @param c:             motion character
/// @param expandMode:    1 for 'i', 2 for 'a'
/// @param first, second: Dynamic arrays in which the prefix and postfix
///                       commands will be returned
/// @return               whether the command could be extracted successfully.
static bool expandExpression(char const c, enum Infix expandMode,
		DynamicArray *first, DynamicArray *second) {
	empty(first);
	empty(second);
	// Motions
	char const lower = tolower(c);
	if (lower == 'w') {
		// translated into wb[command]e resp. WB[command]E, which works
		// file even when at the fist letter. Does not work for single
		// letter words though.
		int const diff = c - lower;
		checkSetNextV(first, c);
		checkSetNextV(first, (signed char)(((int)'b') + diff));
		checkSetNextV(second, (signed char)(((int)'e') + diff));
		return true;
	}
	// Symmetrical brackets (quotation marks)
	if (c == '\'' || c == '"') {
		if (TLINE(term.c.y)[term.c.x].u == c) {
			// Local ambiguity -> do nothing. It cannot be
			// determined if the current character is the first
			// character of the selection or the second one.
			//  <---- search here? -- ['] -- or search here? --->
			return false;
		}
		// ?[c]\nl
		char res [] = {'?', c, '\n'};
		checkSetNextP(first, res);
		checkSetNextV(expandMode == infix_i ? first : second, 'l');
		res[0] = '/';
		checkSetNextP(second, res);
		if (expandMode == infix_i) { checkSetNextV(second, 'h'); }
		return true;
	}
	// Brackets: Does not if in range / if the brackets belong togehter.
	for (size_t pid = 0; pid < sizeof(Brackets); ++pid) {
		if(Brackets[pid].first == c || Brackets[pid].second == c) {
			if (TLINE(term.c.y)[term.c.x].u == Brackets[pid].first) {
				checkSetNextV(first, 'l');
			}
			checkSetNextV(first, '?');
			checkSetNextV(first, Brackets[pid].first);
			checkSetNextV(first, '\n');
			checkSetNextV(expandMode == infix_i ? first : second, 'l');
			checkSetNextV(second, '/');
			checkSetNextV(second, Brackets[pid].second);
			checkSetNextV(second, '\n');
			if (expandMode == infix_i) { checkSetNextV(second, 'h'); }
			return true;
		}
	}
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

ExitState kpressNormalMode(char const * ksym, int len, bool ctrl, bool esc,
		bool enter, bool backspace) {
	if (ctrl) {
		printf("hier\n");
	}
	// [ESC] or [ENTER] abort resp. finish the current level of operation.
	// Typing 'i' if no operation is currently performed behaves like ESC.
	if (esc || enter || (len == 1 && ksym[0] == 'i' && isMotionFinished()
				&& isOperationFinished())) {
		if (terminateCommand(!enter) ) {
			applyPosition(&stateNormalMode.initialPosition);
			stateNormalMode = defaultNormalMode;
			tfulldirt();
			return finished;
		}
		return success;
	}
	// Search: append to search string, then search & highlight
	if (stateNormalMode.motion.search != none
			&& !stateNormalMode.motion.finished) {
		int8_t const sign = getSearchDirection();
		// Apply start position.
		if (backspace) { // XXX: if a quantifier is subject to removal, it is currently only removed
			               //      from the  command string.
			if (!isEmpty(currentCommand) && !isEmpty(&searchString)) {
				pop(currentCommand);
				pop(&searchString);
			} else if (isEmpty(currentCommand) || isEmpty(&searchString)) {
				empty(&highlights);
				stateNormalMode.motion = defaultNormalMode .motion; //< if typed once more than there are
				selclear();                                         //  letters, the search motion is
				return success;                                             //  terminated
			}
			applyPosition(&stateNormalMode.motion.searchPosition);
		} else {
			if (len > 0) {
				char* kSearch = checkGetNext(&searchString);
				utf8decode(ksym, (Rune*)(kSearch), len);

				char* kCommand = checkGetNext(currentCommand);
				utf8decode(ksym, (Rune*)(kCommand), len);
			}
		}
		if (sign == -1) { moveLetter(1); }
		bool const result = gotoStringAndHighlight(sign);

		if (stateNormalMode.command.op == visual) {
			selextend(term.c.x, term.c.y, term.scr, sel.type, 0);
		} else if  (stateNormalMode.command.op == visualLine) {
			selextend(term.col-1, term.c.y, term.scr, sel.type, 0);
		}
		printCommandString();
		printSearchString();
		return result ? success : failed;
	}

	if (len == 0) { return failed; }

	// 'i' mode enabled, hence the expression is to be expanded:
	// [start_expression(ksym[0])] [operation] [stop_expression(ksym[0])]
	if (stateNormalMode.command.infix != infix_none) {
		DynamicArray prefix = CHAR_ARRAY;
		DynamicArray suffix = CHAR_ARRAY;
		bool const found = expandExpression(ksym[0],
		        stateNormalMode.command.infix, &prefix, &suffix);
		if (!found) {
			stateNormalMode.command.infix = infix_none;
			free(prefix.content);
			free(suffix.content);
			return failed;
		}

		char const operation = stateNormalMode.command.op;
		NormalModeState const st = stateNormalMode;
		TCursor const tc = term.c;
		stateNormalMode.command.op = noop;
		stateNormalMode.command.infix = infix_none;

		bool succ = true;
		replay = true;
		for (int i = 0; i < size(&prefix) && succ; ++i) {
			succ = pressKeys(&prefix.content[i], 1);
		}
		replay = true;
		if (succ) {
			kpressNormalMode(&operation, 1, false, 0, 0, 0);
		}
		replay = true;
		for (int i = 0; i < size(&suffix) && succ; ++i) {
			succ = pressKeys(&suffix.content[i], 1);
		}
		replay = false;

		if (!succ) { // go back to the old position, apply op
			stateNormalMode = st;
			term.c = tc;
		}

		free(prefix.content);
		free(suffix.content);
		return succ ? success : failed;
	}


	// V / v or y take precedence over movement commands.
	switch(ksym[0]) {
		case '.':
		{
			if (!isEmpty(currentCommand)) { toggle = !toggle; empty(currentCommand); }
			return executeCommand(lastCommand) ? success : failed;
		}
		case 'i':
			stateNormalMode.command.infix = infix_i;
			return success;
		case 'a':
			stateNormalMode.command.infix = infix_a;
			return success;
		case 'y': //< Yank mode
		{
			char* kCommand = checkGetNext(currentCommand);
			utf8decode(ksym, (Rune*)(kCommand), len);
			switch(stateNormalMode.command.op) {
				case noop:           //< Start yank mode & set #op
					enableOperation(yank);
					selstart(term.c.x, term.c.y, term.scr, 0);
					emptyCurrentCommand();
					break;
				case visualLine:     //< Complete yank operation
				case visual:
					xsetsel(getsel());     //< yank
					xclipcopy();
					exitCommand();         //< reset command
					break;
				case yank:           //< Complete yank operation as in y#amount j
					selstart(0, term.c.y, term.scr, 0);
					int const origY = term.c.y;
					for (uint32_t i = 1; i < MAX(stateNormalMode.motion.amount, 1); ++i) moveLine(1);
					selextend(term.col-1, term.c.y, term.scr, SEL_RECTANGULAR, 0);
					xsetsel(getsel());
					xclipcopy();
					term.c.y = origY;
					exitCommand();
			}
			printCommandString();
			printSearchString();
			return success;
		}
		case 'v':                //< Visual Mode: Toggle mode.
		case 'V':
		{
			enum Operation op = ksym[0] == 'v' ? visual : visualLine;
			bool assign = stateNormalMode.command.op != op;
			abortCommand();
			if (assign) {
				enableOperation(op);
				char* kCommand = checkGetNext(currentCommand);
				utf8decode(ksym, (Rune*)(kCommand), len);
				if (op == visualLine) {
					selstart(0, term.c.y, term.scr, 0);
					selextend(term.col-1, term.c.y, term.scr, SEL_RECTANGULAR, 0);
				} else {
					selstart(term.c.x, term.c.y, term.scr, 0);
				}
			}
			return success;
		}
	}
	// Perform the movement.
	int32_t sign = -1;    //< whether a command goes 'forward' (1) or 'backward' (-1)
	bool discard = false; //< discard input, as it does not have a meaning.
	bool cmdSuccessful = true;
	switch(ksym[0]) {
		case 'j': sign = 1; FALLTHROUGH
		case 'k':
			term.c.y += sign * MAX(stateNormalMode.motion.amount,1);
			break;
		case 'H': term.c.y = 0;
			  break; //< [numer]H ~ L[number]j is not supported.
		case 'M': term.c.y = term.bot / 2;
			  break;
		case 'L': term.c.y = term.bot;
			  break; //< [numer]L ~ L[number]k is not supported.
		case 'G':  //< Differs from vim, but most useful translation.
			applyPosition(&stateNormalMode.initialPosition);
			break;
		case 'l': sign = 1; FALLTHROUGH
		case 'h':
		{
			int32_t const amount = term.c.x 
				+ sign * MAX(stateNormalMode.motion.amount, 1);
			term.c.x = amount % term.col;
			while (term.c.x < 0) { term.c.x += term.col; }
			term.c.y += floor(1.0 * amount / term.col);
			break;
		}
		case '0':
			if (!stateNormalMode.motion.amount) { term.c.x = 0; }
			else { discard = true; }
			break;
		case '$': term.c.x = term.col-1; 
			  break;
		case 'w': FALLTHROUGH
		case 'W': FALLTHROUGH
		case 'e': FALLTHROUGH
		case 'E': sign = 1; FALLTHROUGH
		case 'B': FALLTHROUGH
		case 'b':
		{
			char const * const wDelim = ksym[0] <= 90 
				? wordDelimLarge : wordDelimSmall;
			uint32_t const wDelimLen = strlen(wDelim);
			
			bool const startSpaceIsSeparator = 
				!(ksym[0] == 'w' || ksym[0] == 'W');
			// Whether to start & end with offset:
			bool const performOffset = startSpaceIsSeparator;
			// Max iteration := One complete hist traversal.
			uint32_t const maxIter = (HISTSIZE+term.row) * term.col;
			// Doesn't work exactly as in vim: Linebreak is 
			// counted as 'normal' separator, hence a jump can 
			// span multiple lines here.
			stateNormalMode.motion.amount = 
				MAX(stateNormalMode.motion.amount, 1);
			for (; stateNormalMode.motion.amount > 0; stateNormalMode.motion.amount--) {
				uint8_t state = 0;
				if (performOffset) { moveLetter(sign); }
				for (uint32_t cIt = 0; cIt ++ < maxIter; moveLetter(sign)) {
					if (startSpaceIsSeparator == contains(TLINE(term.c.y)[term.c.x].u, wDelim, wDelimLen)) {
						if (state == 1) {
							if (performOffset) { moveLetter(-sign); }
							break;
						}
					} else if (state == 0) { state = 1; }
				}
			}
			break;
		}
		case '/': sign = 1; FALLTHROUGH
		case '?':
			  empty(&searchString);
			  stateNormalMode.motion.search = sign == 1 ? forward : backward;
			  stateNormalMode.motion.searchPosition.x = term.c.x;
			  stateNormalMode.motion.searchPosition.y = term.c.y;
			  stateNormalMode.motion.searchPosition.yScr = term.scr;
			  stateNormalMode.motion.finished = false;
			  break;
		case 'n': sign = 1; FALLTHROUGH
		case 'N':
			  toggle = !toggle;
			  emptyCurrentCommand();
			  if (stateNormalMode.motion.search == none) {
				  stateNormalMode.motion.search = forward;
				  stateNormalMode.motion.finished = true;
			  }
			  if (stateNormalMode.motion.search == backward) { sign *= -1; }
			  for (int32_t amount = MAX(stateNormalMode.motion.amount, 1); cmdSuccessful && amount > 0; amount--) {
				  moveLetter(sign);
				  cmdSuccessful = gotoStringAndHighlight(sign);
			  }
			  break;
		case 't':
			  if (sel.type == SEL_REGULAR) {
				  sel.type = SEL_RECTANGULAR;
			  } else {
				  sel.type = SEL_REGULAR;
			  }
			  tsetdirt(sel.nb.y, sel.ne.y);
			  discard = true;
			  break;
		default:
			  discard = true;
			  break;
	}
	bool const isNumber = len == 1 && BETWEEN(ksym[0], 48, 57);
	if (isNumber) { //< record numbers
		discard = false;
		stateNormalMode.motion.amount =
			MIN(SHRT_MAX, stateNormalMode.motion.amount * 10 + ksym[0] - 48);
	} else if (!discard) {
		stateNormalMode.motion.amount = 0;
	}

	if (discard) {
		for (size_t i = 0; i < amountNormalModeShortcuts; ++i) {
			if (ksym[0] == normalModeShortcuts[i].key) {
				cmdSuccessful = pressKeys(normalModeShortcuts[i].value, strlen(normalModeShortcuts[i].value));
			}
		}
	} else {
		char* kCommand = checkGetNext(currentCommand);
		utf8decode(ksym, (Rune*)(kCommand), len);

		int diff = 0;
		if (term.c.y > 0) {
			if (term.c.y > term.bot) {
				diff = term.bot - term.c.y;
				term.c.y = term.bot;
			}
		} else {
			if (term.c.y < 0) {
				diff = -term.c.y;
				term.c.y = 0;
			}
		}

		int const _newScr = term.scr + diff;
		term.c.y = _newScr < 0 ? 0 : (_newScr >= HISTSIZE ? term.bot : term.c.y);
		term.scr = mod(_newScr, HISTSIZE);

		if (!isEmpty(&highlights)) {
			empty(&highlights);
			highlightStringOnScreen();
		}

		tsetdirt(0, term.row-3);
		printCommandString();
		printSearchString();

		if (stateNormalMode.command.op == visual) {
			selextend(term.c.x, term.c.y, term.scr, sel.type, 0);
		} else if  (stateNormalMode.command.op == visualLine) {
			selextend(term.col-1, term.c.y, term.scr, sel.type, 0);
		} else {
			if (!isNumber && (stateNormalMode.motion.search == none
					|| stateNormalMode.motion.finished)) {
				toggle = !toggle;
				emptyCurrentCommand();
			}
			if (stateNormalMode.command.op == yank) {
				if (!isNumber && !discard && stateNormalMode.motion.search == none) {
					// copy
					selextend(term.c.x, term.c.y, term.scr, sel.mode, 0);
					xsetsel(getsel());
					xclipcopy();
					applyPosition(&stateNormalMode.command.startPosition);
					exitCommand();
				}
			}
		}
	}
	return cmdSuccessful ? success : failed;
}

 void onMove(void) {
	stateNormalMode.initialPosition.x = term.c.x;
	stateNormalMode.initialPosition.y = term.c.y;
	stateNormalMode.initialPosition.yScr = term.scr;
}

