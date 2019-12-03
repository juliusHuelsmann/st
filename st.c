//XXX checks:
// - term.screenSize, term.totalSize correctly used
// - getWidht() getHeight() used correctly (if in ALT mode, the current line might 
// be in the buffer
//               chunk
// Check every raw access to buffer or alt: mod applied?

/* See LICENSE for license details. */
#include <ctype.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "st.h"
#include "win.h"

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif

/* Arbitrary sizes */
#define UTF_INVALID   0xFFFD
#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define STR_BUF_SIZ   ESC_BUF_SIZ
#define STR_ARG_SIZ   ESC_ARG_SIZ

/* macros */
#define IS_SET(flag)		((term.mode & (flag)) != 0)
#define ISCONTROLC0(c)		(BETWEEN(c, 0, 0x1f) || (c) == '\177')
#define ISCONTROLC1(c)		(BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c)		(ISCONTROLC0(c) || ISCONTROLC1(c))
#define ISDELIM(u)		(u && wcschr(worddelimiters, u))

enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_ALTSCREEN   = 1 << 2,
	MODE_CRLF        = 1 << 3,
	MODE_ECHO        = 1 << 4,
	MODE_PRINT       = 1 << 5,
	MODE_UTF8        = 1 << 6,
	MODE_SIXEL       = 1 << 7,
};

enum cursor_movement {
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum cursor_state {
	CURSOR_DEFAULT  = 0,
	CURSOR_WRAPNEXT = 1,
	CURSOR_ORIGIN   = 2
};

enum charset {
	CS_GRAPHIC0,
	CS_GRAPHIC1,
	CS_UK,
	CS_USA,
	CS_MULTI,
	CS_GER,
	CS_FIN
};

enum escape_state {
	ESC_START      = 1,
	ESC_CSI        = 2,
	ESC_STR        = 4,  /* OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END    = 16, /* a final string was encountered */
	ESC_TEST       = 32, /* Enter in test mode */
	ESC_UTF8       = 64,
	ESC_DCS        =128,
};

typedef struct Position {
	int32_t x;
	int32_t y;
} Position;

typedef Position Size;

typedef struct {
	Glyph attr; /* current char attributes */
	int x;
	int y;
	Position screenOffset;
	char state;
} TCursor;

typedef struct {
	int mode;
	int type;
	int snap;
	/*
	 * Selection variables:
	 * nb – normalized coordinates of the beginning of the selection
	 * ne – normalized coordinates of the end of the selection
	 * ob – original coordinates of the beginning of the selection
	 * oe – original coordinates of the end of the selection
	 */
	struct {
		int x, y;
	} nb, ne, ob, oe;

	int alt;
} Selection;

/// Maximum amount of rows that can be allocated for the main buffer.
static int const maxAmountRows = 1000;

/* Internal representation of the screen */
typedef struct {
	// Size of the different buffers:
	Size totalSize;  // size including hist  -> size of the #buffer  
	Size screenSize; // Screen size <-> size of the alternate screen.
	// Buffers.
	Line *buffer; // normal screen buffer.              totalSize
	Line *alt;    // alternate screen.                 screenSize
	int  *dirty;  // 1 iff a line has to be repainted. screenSize
	int  *tabs;   //                                    totalSize.x
	// Positions (total)
	TCursor c;       // Insert cursor
	TCursor cNorm; // Browse cursor
	// Positions (relative to screenOffset)
	Position oldCursor;
	// Page margins used in order to specify a scroll region.
	int top;      /* top    scroll limit */
	int bot;      /* bottom scroll limit */
	int mode;     /* terminal mode flags */
	int esc;      /* escape state flags */
	char trantbl[4]; /* charset table translation */
	int charset;  /* current charset */
	int icharset; /* selected charset for sequence */
} Term;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]] */
typedef struct {
	char buf[ESC_BUF_SIZ]; /* raw string */
	int len;               /* raw string length */
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg;              /* nb of args */
	char mode[2];
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
	char type;             /* ESC type ... */
	char buf[STR_BUF_SIZ]; /* raw string */
	int len;               /* raw string length */
	char *args[STR_ARG_SIZ];
	int narg;              /* nb of args */
} STREscape;

static void execsh(char *, char **);
static void stty(char **);
static void sigchld(int);
static void ttywriteraw(const char *, size_t);

static void csidump(void);
static void csihandle(void);
static void csiparse(void);
static void csireset(void);
static int eschandle(uchar);
static void strdump(void);
static void strhandle(void);
static void strparse(void);
static void strreset(void);

static void tprinter(char *, size_t);
static void tdumpsel(void);
/// Dump the current line to the primary aux device. 
static void tdumpline(int absoluteLine);
/// Dump all the currently visible lines (in the domain of the insert cursor) 
//to the p rimary aux device.
static void tdump(void);
/// Clear the region in the buffer (Absolute coordinates) and set dirt for the 
/// visible area
static void tclearregion(int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
/// Insert \p n blank characters at the current position at the cursor and 
/// shift the previous content backwards. The 
static void tinsertblank(int n);
/// Same as #tinsertblank but for lines. 
static void tinsertblankline(int);
static int tlinelen(int);
static void tmoveto(TCursor *c, int x, int y, bool shift);
static void tmoveato(int, int);
static void tnewline(int);
static void tputtab(int);
static void tputc(Rune);
static void treset(void);
static void tscrollup(int, int);
static void tscrolldown(int, int);
static void tsetattr(int *, int);
static void tsetchar(Rune); //, Glyph *, int, int);
/// position on screen.
static void tsetdirt(int, int);
static void tsetscroll(int, int);
static void tswapscreen(void);
static void tsetmode(int, int, int *, int);
static int twrite(const char *, int, int);
static void tfulldirt(void);
static void tcontrolcode(uchar );
static void tdectest(char );
static void tdefutf8(char);
static int32_t tdefcolor(int *, int *, int);
static void tdeftran(char);
static void tstrsequence(uchar);
/// Draw the lines screen[x:xe, y:ye] on the current screen if dirty.
static void drawregion(int x, int y, int xe, int ye, Position pos);

static void selnormalize(void);
static void selscroll(int, int);
static void selsnap(int *, int *, int);

static size_t utf8decode(const char *, Rune *, size_t);
static Rune utf8decodebyte(char, size_t *);
static char utf8encodebyte(Rune, size_t);
static size_t utf8validate(Rune *, size_t);

static char *base64dec(const char *);
static char base64dec_getc(const char **);

static ssize_t xwrite(int, const char *, size_t);

/* Globals */
static Term term;
static Selection sel;
static CSIEscape csiescseq;
static STREscape strescseq;
static int iofd = 1;
static int cmdfd;
static pid_t pid;

static uchar utfbyte[UTF_SIZ + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static uchar utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static Rune utfmin[UTF_SIZ + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static Rune utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

static inline bool 
isSet(enum term_mode flag) { return (term.mode & flag) != 0; }

/// Yield buffer dimensions depending on whether ALT screen is set
static inline Size 
getSize(int yAbs) { 
	return  isSet(MODE_ALTSCREEN) && yAbs >= 0 && yAbs < term.screenSize.y 
		? term.screenSize : term.totalSize;
}

static inline int 
getWidth(int yAbs) { 
	return  isSet(MODE_ALTSCREEN) && yAbs >= 0 && yAbs < term.screenSize.y 
		? term.screenSize.x : term.totalSize.x;
}

static inline int 
getHeight() { 
	return isSet(MODE_ALTSCREEN) ? term.screenSize.y 
		+ (!isInsertCursor() ? 0 : term.totalSize.y): term.totalSize.y; 
} 

void
normalModeStart(void) { 
	term.cNorm = term.c;
}


/// Yield the line depending on whether ALT screen is set
static Line 
getLine(int yPosAbsolute) {
	if (isSet(MODE_ALTSCREEN)) { 
		if (yPosAbsolute >= 0 && yPosAbsolute < term.screenSize.y) { 
			return term.alt[yPosAbsolute]; 
		}
		yPosAbsolute += term.screenSize.y;
	}
	return term.buffer[yPosAbsolute % term.totalSize.y];
}
static Line *getLinePtr(int yPosAbsolute) {
	if (isSet(MODE_ALTSCREEN)) { 
		if (yPosAbsolute >= 0 && yPosAbsolute < term.screenSize.y) { 
			return term.alt + yPosAbsolute; 
		}
		yPosAbsolute -= term.screenSize.y;
	}
	return term.buffer + (yPosAbsolute % term.totalSize.y);
}

/// Functions that allow for error reporting and -handling 
static inline void 
assertInScreen(int x, int y) { 
	assert(x >= 0 && x<term.screenSize.x && y>=0 && y<term.screenSize.y);
}
static inline void 
assertInRegion(int x, int y) {
	assert(x >= 0 && x < term.totalSize.x && y >= 0 && term.totalSize.y);
}

ssize_t
xwrite(int fd, const char *s, size_t len)
{
	size_t aux = len;
	ssize_t r;

	while (len > 0) {
		r = write(fd, s, len);
		if (r < 0)
			return r;
		len -= r;
		s += r;
	}

	return aux;
}

void *
xmalloc(size_t len)
{
	void *p;

	if (!(p = malloc(len)))
		die("malloc: %s\n", strerror(errno));

	return p;
}

void *
xrealloc(void *p, size_t len)
{
	if ((p = realloc(p, len)) == NULL)
		die("realloc: %s\n", strerror(errno));

	return p;
}

char *
xstrdup(char *s)
{
	if ((s = strdup(s)) == NULL)
		die("strdup: %s\n", strerror(errno));

	return s;
}

size_t
utf8decode(const char *c, Rune *u, size_t clen)
{
	size_t i, j, len, type;
	Rune udecoded;

	*u = UTF_INVALID;
	if (!clen)
		return 0;
	udecoded = utf8decodebyte(c[0], &len);
	if (!BETWEEN(len, 1, UTF_SIZ))
		return 1;
	for (i = 1, j = 1; i < clen && j < len; ++i, ++j) {
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if (type != 0)
			return j;
	}
	if (j < len)
		return 0;
	*u = udecoded;
	utf8validate(u, len);

	return len;
}

Rune
utf8decodebyte(char c, size_t *i)
{
	for (*i = 0; *i < LEN(utfmask); ++(*i))
		if (((uchar)c & utfmask[*i]) == utfbyte[*i])
			return (uchar)c & ~utfmask[*i];

	return 0;
}

size_t
utf8encode(Rune u, char *c)
{
	size_t len, i;

	len = utf8validate(&u, 0);
	if (len > UTF_SIZ)
		return 0;

	for (i = len - 1; i != 0; --i) {
		c[i] = utf8encodebyte(u, 0);
		u >>= 6;
	}
	c[0] = utf8encodebyte(u, len);

	return len;
}

char
utf8encodebyte(Rune u, size_t i)
{
	return utfbyte[i] | (u & ~utfmask[i]);
}

size_t
utf8validate(Rune *u, size_t i)
{
	if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;

	return i;
}

static const char base64_digits[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0,
	63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0, 0, 0, -1, 0, 0, 0, 0, 1,
	2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
	22, 23, 24, 25, 0, 0, 0, 0, 0, 0, 26, 27, 28, 29, 30, 31, 32, 33, 34,
	35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

char
base64dec_getc(const char **src)
{
	while (**src && !isprint(**src)) (*src)++;
	return *((*src)++);
}

char *
base64dec(const char *src)
{
	size_t in_len = strlen(src);
	char *result, *dst;

	if (in_len % 4)
		in_len += 4 - (in_len % 4);
	result = dst = xmalloc(in_len / 4 * 3 + 1);
	while (*src) {
		int a = base64_digits[(unsigned char) base64dec_getc(&src)];
		int b = base64_digits[(unsigned char) base64dec_getc(&src)];
		int c = base64_digits[(unsigned char) base64dec_getc(&src)];
		int d = base64_digits[(unsigned char) base64dec_getc(&src)];

		*dst++ = (a << 2) | ((b & 0x30) >> 4);
		if (c == -1)
			break;
		*dst++ = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
		if (d == -1)
			break;
		*dst++ = ((c & 0x03) << 6) | d;
	}
	*dst = '\0';
	return result;
}

void 
selinit(void) {
	sel.mode = SEL_IDLE;
	sel.snap = 0;
	sel.ob.x = -1;
}

int 
tlinelen(int yAbsolute) {
	int i = getWidth(yAbsolute);
	if (getLine(yAbsolute)[i - 1].mode & ATTR_WRAP) { return i; }
	while (i > 0 && getLine(yAbsolute)[i - 1].u == ' ') { --i; }
	return i;
}

// TODO: XXX: YYY:
void 
selstart(int xAbsolute, int yAbsolute, int snap) {
	selclear();
	sel.mode = SEL_EMPTY;
	sel.type = SEL_REGULAR;
	sel.alt = IS_SET(MODE_ALTSCREEN);
	sel.snap = snap;
	sel.oe.x = sel.ob.x = xAbsolute;
	sel.oe.y = sel.ob.y = yAbsolute;
	selnormalize();

	if (sel.snap != 0)
		sel.mode = SEL_READY;
	tsetdirt(sel.nb.y, sel.ne.y);
}

// TODO: XXX: YYY:
void 
selextend(int xAbsolute, int yAbsolute, int type, int done) {
	int oldey, oldex, oldsby, oldsey, oldtype;

	if (sel.mode == SEL_IDLE)
		return;
	if (done && sel.mode == SEL_EMPTY) {
		selclear();
		return;
	}

	oldey = sel.oe.y;
	oldex = sel.oe.x;
	oldsby = sel.nb.y;
	oldsey = sel.ne.y;
	oldtype = sel.type;

	sel.oe.x = xAbsolute;
	sel.oe.y = yAbsolute;
	selnormalize();
	sel.type = type;

	if (oldey != sel.oe.y || oldex != sel.oe.x || oldtype != sel.type || sel.mode == SEL_EMPTY)
		tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));

	sel.mode = done ? SEL_IDLE : SEL_READY;
}

// TODO: XXX: YYY:
void 
selnormalize(void) {
	int i;

	if (sel.type == SEL_REGULAR && sel.ob.y != sel.oe.y) {
		sel.nb.x = sel.ob.y < sel.oe.y ? sel.ob.x : sel.oe.x;
		sel.ne.x = sel.ob.y < sel.oe.y ? sel.oe.x : sel.ob.x;
	} else {
		sel.nb.x = MIN(sel.ob.x, sel.oe.x);
		sel.ne.x = MAX(sel.ob.x, sel.oe.x);
	}
	sel.nb.y = MIN(sel.ob.y, sel.oe.y);
	sel.ne.y = MAX(sel.ob.y, sel.oe.y);

	selsnap(&sel.nb.x, &sel.nb.y, -1); // XXX: absolute position
	selsnap(&sel.ne.x, &sel.ne.y, +1); // XXX: absolute position

	/* expand selection over line breaks */
	if (sel.type == SEL_RECTANGULAR)
		return;
	i = tlinelen(sel.nb.y); //XXX: has to be absolute position
	if (i < sel.nb.x)
		sel.nb.x = i;
	if (tlinelen(sel.ne.y) <= sel.ne.x) //XXX: has to be absolute position
		sel.ne.x = term.screenSize.x - 1;
}

int
selected(int xAbsolute, int yAbsolute)
{
	if (sel.mode == SEL_EMPTY || sel.ob.x == -1 ||
			sel.alt != IS_SET(MODE_ALTSCREEN))
		return 0;

	if (sel.type == SEL_RECTANGULAR)
		return BETWEEN(yAbsolute, sel.nb.y, sel.ne.y)
		    && BETWEEN(xAbsolute, sel.nb.x, sel.ne.x);

	return BETWEEN(yAbsolute, sel.nb.y, sel.ne.y)
	    && (yAbsolute != sel.nb.y || xAbsolute >= sel.nb.x)
	    && (yAbsolute != sel.ne.y || xAbsolute <= sel.ne.x);
}

// TODO: XXX: YYY:
void 
selsnap(int *xAbs, int *yAbs, int direction) {

	switch (sel.snap) {
	case SNAP_WORD:
	{
		// Detect the end of the word, and snap around if the word 
		// terminates in a subsequent line.
		// Changed the behavior not to select the entire end of the line 
		Glyph *letter = getLine(*yAbs) + *xAbs; //< not kept in sync.
		bool lineWrap = false;
		for (int xNew = *xAbs; !ISDELIM(letter->u) && !lineWrap; xNew+=direction) {
			if (rlimit(&xNew, 0, term.totalSize.x - 1)) { //< Line Change
				letter = getLine(*yAbs) + xNew;
				lineWrap = letter->mode & ATTR_WRAP;
				if (lineWrap && direction < 0) { break; }  // don't apply current changes
				*yAbs+=direction; 
			} else { letter += direction; }
			*xAbs = xNew;
		}
		break;
	}
	case SNAP_LINE:
		/*
		 * Snap around if the the previous line or the current one
		 * has set ATTR_WRAP at its end. Then the whole next or
		 * previous line will be selected.
		 */
		if (direction < 0) {
			*xAbs = 0;
			for (; *yAbs > 0; *yAbs += direction) {
				if (!(getLine(*yAbs-1)[term.screenSize.x-1].mode
						& ATTR_WRAP)) {
					break;
				}
			}
		} else if (direction > 0) {
			*xAbs = term.screenSize.x -1;
			for (; *yAbs < term.screenSize.y-1; *yAbs += direction) {
				if (!(getLine(*yAbs)[term.screenSize.x-1].mode
						& ATTR_WRAP)) {
					break;
				}
			}
		}
		break;
	}
}

// TODO: XXX: YYY:
char * 
getsel(void) {
	int lastx, linelen;
	Glyph *gp, *last;

	if (sel.ob.x == -1) { return NULL; }
	assert(sel.ne.y >= sel.nb.y);

	int bufsize = (term.screenSize.x+1) * (sel.ne.y-sel.nb.y+1) * UTF_SIZ;
	char *str, *ptr;
	ptr = str = xmalloc(bufsize);

	/* append every set & selected glyph to the selection */
	for (int y = sel.nb.y; y <= sel.ne.y; y++) {
		if ((linelen = tlinelen(y)) == 0) {
			*ptr++ = '\n';
			continue;
		}

		if (sel.type == SEL_RECTANGULAR) {
			gp = &getLine(y)[sel.nb.x];
			lastx = sel.ne.x;
		} else {
			gp = &getLine(y)[sel.nb.y == y ? sel.nb.x : 0];
			lastx = (sel.ne.y == y) ? sel.ne.x : term.screenSize.x-1;
		}
		last = &getLine(y)[MIN(lastx, linelen-1)];
		while (last >= gp && last->u == ' ')
			--last;

		for ( ; gp <= last; ++gp) {
			if (gp->mode & ATTR_WDUMMY)
				continue;

			ptr += utf8encode(gp->u, ptr);
		}

		/*
		 * Copy and pasting of line endings is inconsistent
		 * in the inconsistent terminal and GUI world.
		 * The best solution seems like to produce '\n' when
		 * something is copied from st and convert '\n' to
		 * '\r', when something to be pasted is received by
		 * st.
		 * FIXME: Fix the computer world.
		 */
		if ((y < sel.ne.y || lastx >= linelen) && !(last->mode & ATTR_WRAP))
			*ptr++ = '\n';
	}
	*ptr = 0;
	return str;
}

void 
selclear(void) {
	if (sel.ob.x == -1) { return; }
	sel.mode = SEL_IDLE;
	sel.ob.x = -1;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void 
die(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

void 
execsh(char *cmd, char **args) {
	char *sh, *prog;
	const struct passwd *pw;

	errno = 0;
	if ((pw = getpwuid(getuid())) == NULL) {
		if (errno)
			die("getpwuid: %s\n", strerror(errno));
		else
			die("who are you?\n");
	}

	if ((sh = getenv("SHELL")) == NULL)
		sh = (pw->pw_shell[0]) ? pw->pw_shell : cmd;

	if (args)
		prog = args[0];
	else if (utmp)
		prog = utmp;
	else
		prog = sh;
	DEFAULT(args, ((char *[]) {prog, NULL}));

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", sh, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", termname, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	execvp(prog, args);
	_exit(1);
}

void 
sigchld(int a) {
	int stat;
	pid_t p;

	if ((p = waitpid(pid, &stat, WNOHANG)) < 0)
		die("waiting for pid %hd failed: %s\n", pid, strerror(errno));

	if (pid != p)
		return;

	if (WIFEXITED(stat) && WEXITSTATUS(stat))
		die("child exited with status %d\n", WEXITSTATUS(stat));
	else if (WIFSIGNALED(stat))
		die("child terminated due to signal %d\n", WTERMSIG(stat));
	exit(0);
}

void 
stty(char **args) {
	char cmd[_POSIX_ARG_MAX], **p, *q, *s;
	size_t n, siz;

	if ((n = strlen(stty_args)) > sizeof(cmd)-1)
		die("incorrect stty parameters\n");
	memcpy(cmd, stty_args, n);
	q = cmd + n;
	siz = sizeof(cmd) - n;
	for (p = args; p && (s = *p); ++p) {
		if ((n = strlen(s)) > siz-1)
			die("stty parameter length too long\n");
		*q++ = ' ';
		memcpy(q, s, n);
		q += n;
		siz -= n + 1;
	}
	*q = '\0';
	if (system(cmd) != 0)
		perror("Couldn't call stty");
}

int 
ttynew(char *line, char *cmd, char *out, char **args) {
	int m, s;

	if (out) {
		term.mode |= MODE_PRINT;
		iofd = (!strcmp(out, "-")) ?
			  1 : open(out, O_WRONLY | O_CREAT, 0666);
		if (iofd < 0) {
			fprintf(stderr, "Error opening %s:%s\n",
				out, strerror(errno));
		}
	}

	if (line) {
		if ((cmdfd = open(line, O_RDWR)) < 0)
			die("open line '%s' failed: %s\n",
			    line, strerror(errno));
		dup2(cmdfd, 0);
		stty(args);
		return cmdfd;
	}

	/* seems to work fine on linux, openbsd and freebsd */
	if (openpty(&m, &s, NULL, NULL, NULL) < 0)
		die("openpty failed: %s\n", strerror(errno));

	switch (pid = fork()) {
	case -1:
		die("fork failed: %s\n", strerror(errno));
		break;
	case 0:
		close(iofd);
		setsid(); /* create a new process group */
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		if (ioctl(s, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		close(s);
		close(m);
#ifdef __OpenBSD__
		if (pledge("stdio getpw proc exec", NULL) == -1)
			die("pledge\n");
#endif
		execsh(cmd, args);
		break;
	default:
#ifdef __OpenBSD__
		if (pledge("stdio rpath tty proc", NULL) == -1)
			die("pledge\n");
#endif
		close(s);
		cmdfd = m;
		signal(SIGCHLD, sigchld);
		break;
	}
	return cmdfd;
}

size_t 
ttyread(void) {
	static char buf[BUFSIZ];
	static int buflen = 0;
	int written;
	int ret;

	/* append read bytes to unprocessed bytes */
	if ((ret = read(cmdfd, buf+buflen, LEN(buf)-buflen)) < 0)
		die("couldn't read from shell: %s\n", strerror(errno));
	buflen += ret;

	written = twrite(buf, buflen, 0);
	buflen -= written;
	/* keep any uncomplete utf8 char for the next call */
	if (buflen > 0)
		memmove(buf, buf + written, buflen);

	return ret;
}

void 
ttywrite(const char *s, size_t n, int may_echo) {
	const char *next;

	if (may_echo && IS_SET(MODE_ECHO))
		twrite(s, n, 1);

	if (!IS_SET(MODE_CRLF)) {
		ttywriteraw(s, n);
		return;
	}

	/* This is similar to how the kernel handles ONLCR for ttys */
	while (n > 0) {
		if (*s == '\r') {
			next = s + 1;
			ttywriteraw("\r\n", 2);
		} else {
			next = memchr(s, '\r', n);
			DEFAULT(next, s + n);
			ttywriteraw(s, next - s);
		}
		n -= next - s;
		s = next;
	}
}

void 
ttywriteraw(const char *s, size_t n) {
	fd_set wfd, rfd;
	ssize_t r;
	size_t lim = 256;

	/*
	 * Remember that we are using a pty, which might be a modem line.
	 * Writing too much will clog the line. That's why we are doing this
	 * dance.
	 * FIXME: Migrate the world to Plan 9.
	 */
	while (n > 0) {
		FD_ZERO(&wfd);
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &wfd);
		FD_SET(cmdfd, &rfd);

		/* Check if we can write. */
		if (pselect(cmdfd+1, &rfd, &wfd, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			die("select failed: %s\n", strerror(errno));
		}
		if (FD_ISSET(cmdfd, &wfd)) {
			/*
			 * Only write the bytes written by ttywrite() or the
			 * default of 256. This seems to be a reasonable value
			 * for a serial line. Bigger values might clog the I/O.
			 */
			if ((r = write(cmdfd, s, (n < lim)? n : lim)) < 0)
				goto write_error;
			if (r < n) {
				/*
				 * We weren't able to write out everything.
				 * This means the buffer is getting full
				 * again. Empty it.
				 */
				if (n < lim)
					lim = ttyread();
				n -= r;
				s += r;
			} else {
				/* All bytes have been written. */
				break;
			}
		}
		if (FD_ISSET(cmdfd, &rfd))
			lim = ttyread();
	}
	return;

write_error:
	die("write error on tty: %s\n", strerror(errno));
}

void 
ttyresize(int tw, int th) {
	struct winsize w;

	w.ws_row = term.screenSize.y;
	w.ws_col = term.screenSize.x;
	w.ws_xpixel = tw;
	w.ws_ypixel = th;
	if (ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

void 
ttyhangup() { /* Send SIGHUP to shell */
	kill(pid, SIGHUP);
}

void 
tsetdirt(int startY, int stopY) {
	LIMIT(startY, 0, term.screenSize.y-1);
	LIMIT(stopY,  0, term.screenSize.y-1);

	for (int i = startY; i <= stopY; i++) { term.dirty[i] = 1; }
}

int 
tattrset(int attr, int dirt) {
	TCursor const *activeCursor = isInsertCursor() ? &term.c : &term.cNorm;
	// TODO: why not the last line / char???
	int const maxy = min(term.screenSize.y + activeCursor->screenOffset.y, 
			getHeight()) - 1;
	for (int i = activeCursor->screenOffset.y; i < maxy; i++) {
		int const maxx = min(term.screenSize.x + 
				activeCursor->screenOffset.x, getWidth(i)) - 1;
		for (int j = activeCursor->screenOffset.x; j < maxx; j++) { 
			if (getLine(i)[j].mode & attr) {
				if (dirt) { tsetdirt(i, i); }
				return 1;
			}
		}
	}
	return 0;
}

void 
tfulldirt(void) {
	tsetdirt(0, term.screenSize.y-1);
}

void
tcursor(int mode)
{
	static TCursor c[2];
	int alt = IS_SET(MODE_ALTSCREEN);

	if (mode == CURSOR_SAVE) {
		c[alt] = term.c;
	} else if (mode == CURSOR_LOAD) {
		term.c = c[alt];
		tmoveto(&term.c, term.c.x, term.c.y, !alt); // or false in general.
	}
}

void
treset(void)
{
	uint i;

	term.c = (TCursor){
		.attr= { .mode = ATTR_NULL, .fg = defaultfg, .bg = defaultbg }, 
		.x = 0, .y = 0, .state = CURSOR_DEFAULT};

	memset(term.tabs, 0, term.screenSize.x * sizeof(*term.tabs));
	for (i = tabspaces; i < term.screenSize.x; i += tabspaces)
		term.tabs[i] = 1;

	term.top = 0;
	term.bot = term.screenSize.y - 1;

	term.mode = MODE_WRAP|MODE_UTF8;
	memset(term.trantbl, CS_USA, sizeof(term.trantbl));
	term.charset = 0;

	selclear();
	term.cNorm = (TCursor) { .attr = term.c.attr, .x = 0, .y = 0, .state = CURSOR_DEFAULT};
	term.cNorm.y = 0;
	term.cNorm.screenOffset.x = 0;
	term.cNorm.screenOffset.y = 0;


	for (i = 0; i < 2; i++) {
		term.c.screenOffset.x = 0;
		term.c.screenOffset.y = 0;
		tmoveto(&term.c, 0, 0, false);
		tcursor(CURSOR_SAVE);                       // save the cursor
		tclearregion(0, 0, term.screenSize.x-1, term.screenSize.y-1); // manually reset the visual part of the buffer.
		tswapscreen();                              // switch from ALT screen to normal scren mode.
	}
}

void 
tnew(int col, int row) {
	printf("[DBG]new %d, %d\n", row, col);
	term = (Term){ .c = { .attr = { .fg = defaultfg, .bg = defaultbg } } };
	term.buffer = malloc(maxAmountRows * sizeof(*term.buffer));
	tresize(col, row);
	treset();
}

void 
tswapscreen(void) {
	term.mode ^= MODE_ALTSCREEN;
	tcursor(CURSOR_LOAD);
	tfulldirt();
}

/// [..... yPosOrig...bottom] -> [....[n x empty][yPosOrig...bottom-n]
void 
tscrolldown(int yPosOrig, int n) {
	if (n <= 0) { return; }
	LIMIT(term.bot, 0, term.screenSize.y);
	LIMIT(term.top, 0, term.screenSize.y);
	bool const alt = isSet(MODE_ALTSCREEN);
	int const yRelative = yPosOrig - term.c.screenOffset.y;
	assert(!alt || (term.c.screenOffset.y == 0));
	assert(term.bot >= term.top);
	assert(yRelative >= term.top && yRelative < term.bot);

	n = min(n, term.bot - yPosOrig);
	// move the lines in [orig:bottom-n] n lines down
	// buffer[yPosOrig+n:term.bot] <- buffer[yPosOrig:term.bot-n]
	int const end = yPosOrig + n + term.c.screenOffset.y;
	for (int line = term.bot + term.c.screenOffset.y; line >= end; --line) {
		Line temp = getLine(line);
		*getLinePtr(line) = getLine(line - n);
		*getLinePtr(line - n) = temp;
	}
	// clear the 'new' region and set dirt to [orig, term.bot] (tclearregion sets dirt aswell)
	tclearregion(0, yPosOrig, getWidth(0) - 1, yPosOrig + n);
	tsetdirt(yRelative + n, term.bot);
	// Adjust the selection if in that region.
	selscroll(yPosOrig, n);
}

void 
tscrollup(int yPosOrig, int n) {
	if (n <= 0) { return; }
	LIMIT(term.bot, 0, term.screenSize.y);
	LIMIT(term.top, 0, term.screenSize.y);
	bool const alt = isSet(MODE_ALTSCREEN);
	int const yRelative = yPosOrig - term.c.screenOffset.y;
	assert(!alt || (term.c.screenOffset.y == 0));
	assert(term.bot >= term.top);
	assert(yRelative >= term.top && yRelative < term.bot);

	n = min(n, term.bot - yPosOrig);
	// buffer[orig:term.bot-n] <- buffer[orig+n:bot]
	int const end = term.bot - n + term.c.screenOffset.y;
	for (int line = yPosOrig + term.c.screenOffset.y; line <= end; ++line) {
		Line temp = getLine(line);
		*getLinePtr(line) = getLine(line + n);
		*getLinePtr(line + n) = temp;
	}
	tclearregion(0, term.c.screenOffset.y + term.bot - n, getWidth(0) - 1, 
			term.bot + term.c.screenOffset.y);
	tsetdirt(yRelative, term.bot - n); // XXX:
	selscroll(yPosOrig, -n);
}

void 
selscroll(int orig, int n) {
	if (sel.ob.x == -1) { return; }

	// XXX:
	if (BETWEEN(sel.ob.y, orig, term.bot) || BETWEEN(sel.oe.y, orig, term.bot)) {
		if ((sel.ob.y += n) > term.bot || (sel.oe.y += n) < term.top) {
			selclear();
			return;
		}
		if (sel.type == SEL_RECTANGULAR) {
			if (sel.ob.y < term.top)
				sel.ob.y = term.top;
			if (sel.oe.y > term.bot)
				sel.oe.y = term.bot;
		} else {
			if (sel.ob.y < term.top) {
				sel.ob.y = term.top;
				sel.ob.x = 0;
			}
			if (sel.oe.y > term.bot) {
				sel.oe.y = term.bot;
				sel.oe.x = term.screenSize.x;
			}
		}
		selnormalize();
	}
}

void 
tnewline(int first_col) {
	int y = term.c.y;
	bool const alt = isSet(MODE_ALTSCREEN);
	if (!alt) {
		// XXX: expand
		if (y == term.totalSize.y - 1) {
			//term.totalSize.y;

		}
	}
	tmoveto(&term.c, first_col ? 0 : term.c.x, term.c.y + 1, true);
	// emptying the line is only necessary for the non alt screen, and should be performed by 
	// the tmoveto function.
	// XXX: callocate new line
}

void
csiparse(void)
{
	char *p = csiescseq.buf, *np;
	long int v;

	csiescseq.narg = 0;
	if (*p == '?') {
		csiescseq.priv = 1;
		p++;
	}

	csiescseq.buf[csiescseq.len] = '\0';
	while (p < csiescseq.buf+csiescseq.len) {
		np = NULL;
		v = strtol(p, &np, 10);
		if (np == p)
			v = 0;
		if (v == LONG_MAX || v == LONG_MIN)
			v = -1;
		csiescseq.arg[csiescseq.narg++] = v;
		p = np;
		if (*p != ';' || csiescseq.narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	csiescseq.mode[0] = *p++;
	csiescseq.mode[1] = (p < csiescseq.buf+csiescseq.len) ? *p : '\0';
}

/* for absolute user moves, when decom is set */
void 
tmoveato(int x, int y) {
	tmoveto(&term.c, x + term.c.screenOffset.x,
			y + term.c.screenOffset.y + 
			((term.c.state & CURSOR_ORIGIN) ? term.top: 0), false);
}

void 
tmoveto(TCursor *t, int newx, int newy, bool move) {
	int miny, maxy; // Figure out on-screen boundraries
	if (t->state & CURSOR_ORIGIN) {
		miny = MAX(term.top, 0);
		maxy = MIN(term.bot, term.screenSize.y);
	} else {
		miny = 0;
		maxy = term.screenSize.y - 1;
	}

	t->state &= ~CURSOR_WRAPNEXT;
	if (move) {
		if (isSet(MODE_ALTSCREEN) && t == &term.c) {
			assert(!t->screenOffset.x && !t->screenOffset.y);
			if (newy < term.top) {
				tscrolldown(term.top, term.top - newy);
				t->y = term.top;
			} else if (newy > term.bot) {
				tscrollup(term.top, newy - term.bot);
				t->y = term.bot;
			} else {
				t->y = LIMIT(newy, miny, maxy);
			}
			t->x = LIMIT(newx, 0, term.screenSize.x - 1);
		} else {
			assert(miny <= maxy);
			// Allocate new memory if INSERT cursor & exceed size 
			if (&term.c == t) {
				assert(!isSet(MODE_ALTSCREEN));
				size_t const s = term.totalSize.x*sizeof(Glyph);
				int const supAlloc = min(maxAmountRows, newy+1);
				int const ps = term.totalSize.y;
				while (term.totalSize.y < supAlloc) {
					term.buffer[term.totalSize.y++]
						= xmalloc(s);
				}
				tclearregion(0, ps, term.totalSize.x, newy);
				newy %= term.totalSize.y;
			}
			Size const size = getSize(newy);
			assert(size.x && size.y);
			bool const paint = (t == &term.c) == isInsertCursor();
			// Scroll in x direction
			// Fix screen offset if not up to date anymore (resize)
			bool repaintAll = limit(&t->screenOffset.x, 
					0, size.x - term.screenSize.x);
			LIMIT(newx, 0, size.x); // not necessary I guess
			if (newx < t->screenOffset.x) {
				t->x = (t->screenOffset.x = max(newx, 0));
				repaintAll = paint;
			} else if (newx >=t->screenOffset.x+term.screenSize.x) {
				t->screenOffset.x = (t->x=min(newx,size.x-1)) 
					- term.screenSize.x + 1;
				repaintAll = paint;
			} else { t->x = newx; } 
			assert(between(t->x, 0, size.x - 1));
			assert(between(t->screenOffset.x, 0, 
						size.x - term.screenSize.x));
			// Y offset
			LIMIT(t->screenOffset.y, - miny, size.y - maxy - 1);
			// scroll y
			int const mincy = t->screenOffset.y + miny;
			int const maxcy = t->screenOffset.y + maxy;
			if (newy < mincy) { // such that screenOffset + miny = t->y
				t->screenOffset.y = (t->y = max(newy, 0)) - miny;
				repaintAll = paint;
			} else if (newy > maxcy) { // s.th. offset + maxy    = t->y
				t->screenOffset.y = (t->y = min(newy, size.y - 1)) - maxy;
				repaintAll = paint;
			} else { t->y = newy; }
			assert(between(t->y - t->screenOffset.y, miny, maxy));
			assert(t->screenOffset.y >= -miny 
					&& t->screenOffset.y < size.y - maxy);

			// Repaint if necessary and cursor is active
			if (repaintAll) { tfulldirt(); }
		}
	} else {
		t->y = LIMIT(newy, miny + t->screenOffset.y, 
				maxy + t->screenOffset.y);
		t->x = LIMIT(newx, t->screenOffset.x,  
				term.screenSize.x + t->screenOffset.x - 1);
				//getWidth(t->y)-1);
	}
}

void 
tsetchar(Rune u) {//, Glyph *attr, int x, int y) {
	static char *vt100_0[62] = { /* 0x41 - 0x7e */
		"↑", "↓", "→", "←", "█", "▚", "☃", /* A - G */
		0, 0, 0, 0, 0, 0, 0, 0, /* H - O */
		0, 0, 0, 0, 0, 0, 0, 0, /* P - W */
		0, 0, 0, 0, 0, 0, 0, " ", /* X - _ */
		"◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
		"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
		"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
		"│", "≤", "≥", "π", "≠", "£", "·", /* x - ~ */
	};

	/*
	 * The table is proudly stolen from rxvt.
	 */
	if (term.trantbl[term.charset] == CS_GRAPHIC0 &&
	   BETWEEN(u, 0x41, 0x7e) && vt100_0[u - 0x41])
		utf8decode(vt100_0[u - 0x41], &u, UTF_SIZ);

	Line cLine = getLine(term.c.y);
	if (cLine[term.c.x].mode & ATTR_WIDE) {
		if (term.c.x+1 < getWidth(term.c.y)) {
			cLine[term.c.x+1].u = ' ';
			cLine[term.c.x+1].mode &= ~ATTR_WDUMMY;
		}
	} else if (cLine[term.c.x].mode & ATTR_WDUMMY) {
		assert(term.c.x > 0);
		cLine[term.c.x-1].u = ' ';
		cLine[term.c.x-1].mode &= ~ATTR_WIDE;
	}


	int const yRel = term.c.y - term.c.screenOffset.y;
	if (between(yRel, 0, term.screenSize.y - 1)) {
          term.dirty[yRel] = 1;
	}
	cLine[term.c.x] = term.c.attr;
	cLine[term.c.x].u = u;
}

/// Clears the specified region and sets the dirt if the currently altered region is visible
void 
tclearregion(int x1, int y1, int x2, int y2) {
	if (x1 > x2 || y1 > y2) {
	  //printf("no clear x:[%d, %d], y:[%d, %d].\n", x1, x2, y1, y2); 
	  return; 
	}
	assert(x1 >= 0 && x2 >= 0 && y1 >= 0 && y2 >= 0);
	//x1 = max(0, x1); x2 = max(0, x2); y1 = max(0, y1); y2 = max(0, y2);
	assert(!isSet(MODE_ALTSCREEN) || y2 < term.screenSize.y);

	// Figure out what to repaint
	TCursor const *activeCursor = isInsertCursor() ? &term.c : &term.cNorm;
	int const minOnScreen = y1 - activeCursor->screenOffset.y;
	int const maxOnScreen = y2 - activeCursor->screenOffset.y;
	if (maxOnScreen >= 0 && minOnScreen < term.screenSize.y) {
		tsetdirt(max(minOnScreen,0),min(maxOnScreen,term.screenSize.y));
	}
	// fill the content.
	static int bg = 0;
	bg = (bg + 1) % 255;
	int const y = y1;
	printf("[(%d,%d), (%d,%d)]\n", x1, y1, x2, y2);
	for (; y1 <= y2; ++y1) {
		for (int x = x1; x <= min(getWidth(y1) - 1, x2); x++) {
			Glyph* gp = &getLine(y1)[x];
			if (selected(x, y1)) { selclear(); }
			gp->fg = term.c.attr.fg;
			gp->bg = (x1 == x || x2 == x) && (y1 == y || y1 == y2) ? term.c.attr.bg : bg; // :w
			// term.c.attr.bg;
			gp->mode = 0;
			gp->u = ' ';
		}
	}
}

void 
tdeletechar(int n) {
	assert(!isSet(MODE_ALTSCREEN)||between(term.c.y,0,term.screenSize.y-1));
	int const bufferWidth = getWidth(term.c.y);
	LIMIT(n, 0, bufferWidth - term.c.x + term.c.screenOffset.x);

	Line const line = getLine(term.c.y);
	int const dst = term.c.x;
	int const src = dst + n;
	int const size = bufferWidth - src;

	memmove(line + dst, line + src, size * sizeof(Glyph));
	tclearregion(bufferWidth - n, term.c.y, bufferWidth-1, term.c.y);
}

void 
tinsertblank(int n) {
	assert(!isSet(MODE_ALTSCREEN)||between(term.c.y,0,term.screenSize.y-1));
	int const bufferWidth = getWidth(term.c.y);
	LIMIT(n, 0, bufferWidth - term.c.x + term.c.screenOffset.x);

	int const src = term.c.x;
	int const dst = src + n;
	int const size = bufferWidth - dst;
	Glyph * const line = getLine(term.c.y);
	memmove(line + dst, line + src, size * sizeof(Glyph));

	tclearregion(src, term.c.y, dst - 1, term.c.y);
}

void 
tinsertblankline(int n) { // Changes the content of the buffer -> term.c
	if (n <= 0) { return; }
	if (isSet(MODE_ALTSCREEN)) {
		assert(between(term.c.y,0,term.screenSize.y-1));
		assert(!term.c.screenOffset.x&&!term.c.screenOffset.y);
		if (BETWEEN(term.c.y, term.top, term.bot)) { 
			tscrolldown(term.c.y, n);
		}
	} else {
		tclearregion(0, term.c.y, getWidth(0) - 1, term.c.y + n);
	}
}

void
tdeleteline(int n) { // changes the content of the buffer -> term.c
	if (isSet(MODE_ALTSCREEN)) {
		assert(between(term.c.y,0,term.screenSize.y-1));
		assert(!term.c.screenOffset.x && !term.c.screenOffset.y);
		if (BETWEEN(term.c.y, term.top, term.bot)) { 
			tscrollup(term.c.y, n);
		}
	} else {
		tclearregion(0, term.c.screenOffset.y + term.bot - n, 
				getWidth(0)-1, term.bot+term.c.screenOffset.y);
	}
}

int32_t
tdefcolor(int *attr, int *npar, int l)
{
	int32_t idx = -1;
	uint r, g, b;

	switch (attr[*npar + 1]) {
	case 2: /* direct color in RGB space */
		if (*npar + 4 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		r = attr[*npar + 2];
		g = attr[*npar + 3];
		b = attr[*npar + 4];
		*npar += 4;
		if (!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255))
			fprintf(stderr, "erresc: bad rgb color (%u,%u,%u)\n",
				r, g, b);
		else
			idx = TRUECOLOR(r, g, b);
		break;
	case 5: /* indexed color */
		if (*npar + 2 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		*npar += 2;
		if (!BETWEEN(attr[*npar], 0, 255))
			fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
		else
			idx = attr[*npar];
		break;
	case 0: /* implemented defined (only foreground) */
	case 1: /* transparent */
	case 3: /* direct color in CMY space */
	case 4: /* direct color in CMYK space */
	default:
		fprintf(stderr,
		        "erresc(38): gfx attr %d unknown\n", attr[*npar]);
		break;
	}

	return idx;
}

void
tsetattr(int *attr, int l)
{
	int i;
	int32_t idx;

	for (i = 0; i < l; i++) {
		switch (attr[i]) {
		case 0:
			term.c.attr.mode &= ~(
				ATTR_BOLD       |
				ATTR_FAINT      |
				ATTR_ITALIC     |
				ATTR_UNDERLINE  |
				ATTR_BLINK      |
				ATTR_REVERSE    |
				ATTR_INVISIBLE  |
				ATTR_STRUCK     );
			term.c.attr.fg = defaultfg;
			term.c.attr.bg = defaultbg;
			break;
		case 1:
			term.c.attr.mode |= ATTR_BOLD;
			break;
		case 2:
			term.c.attr.mode |= ATTR_FAINT;
			break;
		case 3:
			term.c.attr.mode |= ATTR_ITALIC;
			break;
		case 4:
			term.c.attr.mode |= ATTR_UNDERLINE;
			break;
		case 5: /* slow blink */
			/* FALLTHROUGH */
		case 6: /* rapid blink */
			term.c.attr.mode |= ATTR_BLINK;
			break;
		case 7:
			term.c.attr.mode |= ATTR_REVERSE;
			break;
		case 8:
			term.c.attr.mode |= ATTR_INVISIBLE;
			break;
		case 9:
			term.c.attr.mode |= ATTR_STRUCK;
			break;
		case 22:
			term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
			break;
		case 23:
			term.c.attr.mode &= ~ATTR_ITALIC;
			break;
		case 24:
			term.c.attr.mode &= ~ATTR_UNDERLINE;
			break;
		case 25:
			term.c.attr.mode &= ~ATTR_BLINK;
			break;
		case 27:
			term.c.attr.mode &= ~ATTR_REVERSE;
			break;
		case 28:
			term.c.attr.mode &= ~ATTR_INVISIBLE;
			break;
		case 29:
			term.c.attr.mode &= ~ATTR_STRUCK;
			break;
		case 38:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.fg = idx;
			break;
		case 39:
			term.c.attr.fg = defaultfg;
			break;
		case 48:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.bg = idx;
			break;
		case 49:
			term.c.attr.bg = defaultbg;
			break;
		default:
			if (BETWEEN(attr[i], 30, 37)) {
				term.c.attr.fg = attr[i] - 30;
			} else if (BETWEEN(attr[i], 40, 47)) {
				term.c.attr.bg = attr[i] - 40;
			} else if (BETWEEN(attr[i], 90, 97)) {
				term.c.attr.fg = attr[i] - 90 + 8;
			} else if (BETWEEN(attr[i], 100, 107)) {
				term.c.attr.bg = attr[i] - 100 + 8;
			} else {
				fprintf(stderr,
					"erresc(default): gfx attr %d unknown\n",
					attr[i]);
				csidump();
			}
			break;
		}
	}
}

void 
tsetscroll(int t, int b) {
	LIMIT(t, 0, term.screenSize.y-1);
	LIMIT(b, 0, term.screenSize.y-1);
	assert(b >= t);
	term.top = t;
	term.bot = b;
}

void
tsetmode(int priv, int set, int *args, int narg)
{
	int alt, *lim;

	for (lim = args + narg; args < lim; ++args) {
		if (priv) {
			switch (*args) {
			case 1: /* DECCKM -- Cursor key */
				xsetmode(set, MODE_APPCURSOR);
				break;
			case 5: /* DECSCNM -- Reverse video */
				xsetmode(set, MODE_REVERSE);
				break;
			case 6: /* DECOM -- Origin */
				MODBIT(term.c.state, set, CURSOR_ORIGIN);
				tmoveato(0, 0);
				break;
			case 7: /* DECAWM -- Auto wrap */
				MODBIT(term.mode, set, MODE_WRAP);
				break;
			case 0:  /* Error (IGNORED) */
			case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
			case 3:  /* DECCOLM -- Column  (IGNORED) */
			case 4:  /* DECSCLM -- Scroll (IGNORED) */
			case 8:  /* DECARM -- Auto repeat (IGNORED) */
			case 18: /* DECPFF -- Printer feed (IGNORED) */
			case 19: /* DECPEX -- Printer extent (IGNORED) */
			case 42: /* DECNRCM -- National characters (IGNORED) */
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				break;
			case 25: /* DECTCEM -- Text Cursor Enable Mode */
				xsetmode(!set, MODE_HIDE);
				break;
			case 9:    /* X10 mouse compatibility mode */
				xsetpointermotion(0);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEX10);
				break;
			case 1000: /* 1000: report button press */
				xsetpointermotion(0);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEBTN);
				break;
			case 1002: /* 1002: report motion on button press */
				xsetpointermotion(0);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEMOTION);
				break;
			case 1003: /* 1003: enable all mouse motions */
				xsetpointermotion(set);
				xsetmode(0, MODE_MOUSE);
				xsetmode(set, MODE_MOUSEMANY);
				break;
			case 1004: /* 1004: send focus events to tty */
				xsetmode(set, MODE_FOCUS);
				break;
			case 1006: /* 1006: extended reporting mode */
				xsetmode(set, MODE_MOUSESGR);
				break;
			case 1034:
				xsetmode(set, MODE_8BIT);
				break;
			case 1049: /* swap screen & set/restore cursor as xterm */
				if (!allowaltscreen)
					break;
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
				/* FALLTHROUGH */
			case 47: /* swap screen */
			case 1047:
				if (!allowaltscreen)
					break;
				alt = IS_SET(MODE_ALTSCREEN);
				if (alt) {
					tclearregion(0, 0, term.screenSize.x-1, term.screenSize.y-1);
				}
				if (set ^ alt) /* set is always 1 or 0 */
					tswapscreen();
				if (*args != 1049)
					break;
				/* FALLTHROUGH */
			case 1048:
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
				break;
			case 2004: /* 2004: bracketed paste mode */
				xsetmode(set, MODE_BRCKTPASTE);
				break;
			/* Not implemented mouse modes. See comments there. */
			case 1001: /* mouse highlight mode; can hang the
				      terminal by design when implemented. */
			case 1005: /* UTF-8 mouse mode; will confuse
				      applications not supporting UTF-8
				      and luit. */
			case 1015: /* urxvt mangled mouse mode; incompatible
				      and can be mistaken for other control
				      codes. */
				break;
			default:
				fprintf(stderr,
					"erresc: unknown private set/reset mode %d\n",
					*args);
				break;
			}
		} else {
			switch (*args) {
			case 0:  /* Error (IGNORED) */
				break;
			case 2:
				xsetmode(set, MODE_KBDLOCK);
				break;
			case 4:  /* IRM -- Insertion-replacement */
				MODBIT(term.mode, set, MODE_INSERT);
				break;
			case 12: /* SRM -- Send/Receive */
				MODBIT(term.mode, !set, MODE_ECHO);
				break;
			case 20: /* LNM -- Linefeed/new line */
				MODBIT(term.mode, set, MODE_CRLF);
				break;
			default:
				fprintf(stderr,
					"erresc: unknown set/reset mode %d\n",
					*args);
				break;
			}
		}
	}
}

// XXX: I'me here currently. todo csi handle and up! :) 
void 
csihandle(void) {
	char buf[40];
	int len;

	switch (csiescseq.mode[0]) {
	default:
	unknown:
		fprintf(stderr, "erresc: unknown csi ");
		csidump();
		/* die(""); */
		break;
	case '@': /* ICH -- Insert <n> blank char */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblank(csiescseq.arg[0]);
		break;
	case 'A': /* CUU -- Cursor <n> Up */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(&term.c, term.c.x, term.c.y - csiescseq.arg[0], false); // XXX: could be changed to !IS_SET(ALT) if 
		break;
	case 'B': /* CUD -- Cursor <n> Down */
	case 'e': /* VPR --Cursor <n> Down */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(&term.c, term.c.x, term.c.y + csiescseq.arg[0], false);
		break;
	case 'i': /* MC -- Media Copy */
		switch (csiescseq.arg[0]) {
		case 0:
			tdump();
			break;
		case 1:
			tdumpline(term.c.y);
			break;
		case 2:
			tdumpsel();
			break;
		case 4:
			term.mode &= ~MODE_PRINT;
			break;
		case 5:
			term.mode |= MODE_PRINT;
			break;
		}
		break;
	case 'c': /* DA -- Device Attributes */
		if (csiescseq.arg[0] == 0)
			ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 'C': /* CUF -- Cursor <n> Forward */
	case 'a': /* HPR -- Cursor <n> Forward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(&term.c, term.c.x + csiescseq.arg[0], term.c.y, false);
		break;
	case 'D': /* CUB -- Cursor <n> Backward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(&term.c, term.c.x - csiescseq.arg[0], term.c.y, false);
		break;
	case 'E': /* CNL -- Cursor <n> Down and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(&term.c, 0, term.c.y+csiescseq.arg[0], false);
		break;
	case 'F': /* CPL -- Cursor <n> Up and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(&term.c, 0, term.c.y-csiescseq.arg[0], false);
		break;
	case 'g': /* TBC -- Tabulation clear */
		switch (csiescseq.arg[0]) {
		case 0: /* clear current tab stop */
			term.tabs[term.c.x] = 0;
			break;
		case 3: /* clear all the tabs */
			memset(term.tabs, 0, term.totalSize.x * sizeof(*term.tabs));
			break;
		default:
			goto unknown;
		}
		break;
	case 'G': /* CHA -- Move to <col> */
	case '`': /* HPA */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(&term.c, csiescseq.arg[0]-1, term.c.y, false); // false in any case here
		break;
	case 'H': /* CUP -- Move to <row> <col> */
	case 'f': /* HVP */
		DEFAULT(csiescseq.arg[0], 1);
		DEFAULT(csiescseq.arg[1], 1);
		tmoveato(csiescseq.arg[1]-1, csiescseq.arg[0]-1);
		break;
	case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(csiescseq.arg[0]);
		break;
	case 'J': /* ED -- Clear screen */
	{
		switch (csiescseq.arg[0]) {
		case 0:
			tclearregion(0, 1 + term.c.y, getWidth(1 + term.c.y)-1,
					term.c.screenOffset.y
					+ term.screenSize.y - 1);
			break;
		case 1: /* above */
			tclearregion(0, term.c.screenOffset.y, 
					getWidth(term.c.screenOffset.y), 
					term.c.y - 1);
			break;
		case 2: /* all */
			tclearregion(0, term.c.screenOffset.y, 
					getWidth(term.c.screenOffset.y) - 1, 
					term.c.screenOffset.y 
					+ term.screenSize.y - 1);
			break;
		default:
			goto unknown;
		}
		// fallthrough
	}
	case 'K': /* EL -- Clear line */
	{
		switch (csiescseq.arg[0]) {
		case 0: /* right */
			tclearregion(term.c.x, term.c.y, 
					getWidth(term.c.y) - 1, term.c.y);
			break;
		case 1: /* left */
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2: /* all */
			tclearregion(0, term.c.y, 
					getWidth(term.c.y) - 1, term.c.y);
			break;
		}
		break;
	}
	case 'S': /* SU -- Scroll <n> line up */
		DEFAULT(csiescseq.arg[0], 1);
		tscrollup(term.top, csiescseq.arg[0]);   //TODO: could be different in the non alt mode.
		break;
	case 'T': /* SD -- Scroll <n> line down */
		DEFAULT(csiescseq.arg[0], 1);
		tscrolldown(term.top, csiescseq.arg[0]); // TODO: could be different in the non alt mode.
		break;
	case 'L': /* IL -- Insert <n> blank lines */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblankline(csiescseq.arg[0]);      // XXX: check
		break;
	case 'l': /* RM -- Reset Mode */
		tsetmode(csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
		break;
	case 'M': /* DL -- Delete <n> lines */
		DEFAULT(csiescseq.arg[0], 1);
		tdeleteline(csiescseq.arg[0]);           // XXX: check
		break;
	case 'X': /* ECH -- Erase <n> char */
	{
		DEFAULT(csiescseq.arg[0], 1);
		tclearregion(term.c.x, term.c.y, term.c.x + csiescseq.arg[0] - 1, term.c.y);           // XXX: check
		break;
	}
	case 'P': /* DCH -- Delete <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tdeletechar(csiescseq.arg[0]);           // XXX: check
		break;
	case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(-csiescseq.arg[0]);
		break;
	case 'd': /* VPA -- Move to <row> */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveato(term.c.x - term.c.screenOffset.x, csiescseq.arg[0]-1);
		break;
	case 'h': /* SM -- Set terminal mode */
		tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
		break;
	case 'm': /* SGR -- Terminal attribute (color) */
		tsetattr(csiescseq.arg, csiescseq.narg);
		break;
	case 'n': /* DSR – Device Status Report (cursor position) */
		if (csiescseq.arg[0] == 6) {
			len = snprintf(buf, sizeof(buf),"\033[%i;%iR",
					term.c.y+1, term.c.x+1);
			ttywrite(buf, len, 0);
		}
		break;
	case 'r': /* DECSTBM -- Set Scrolling Region */
		if (csiescseq.priv) {
			goto unknown;
		} else {
			DEFAULT(csiescseq.arg[0], 1);
			DEFAULT(csiescseq.arg[1], term.screenSize.y);
			tsetscroll(csiescseq.arg[0]-1, csiescseq.arg[1]-1);
			tmoveato(0, 0);
		}
		break;
	case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
		tcursor(CURSOR_SAVE);
		break;
	case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
		tcursor(CURSOR_LOAD);
		break;
	case ' ':
		switch (csiescseq.mode[1]) {
		case 'q': /* DECSCUSR -- Set Cursor Style */
			if (xsetcursor(csiescseq.arg[0]))
				goto unknown;
			break;
		default:
			goto unknown;
		}
		break;
	}
}

void 
csidump(void) {
	int i;
	uint c;

	fprintf(stderr, "ESC[");
	for (i = 0; i < csiescseq.len; i++) {
		c = csiescseq.buf[i] & 0xff;
		if (isprint(c)) {
			putc(c, stderr);
		} else if (c == '\n') {
			fprintf(stderr, "(\\n)");
		} else if (c == '\r') {
			fprintf(stderr, "(\\r)");
		} else if (c == 0x1b) {
			fprintf(stderr, "(\\e)");
		} else {
			fprintf(stderr, "(%02x)", c);
		}
	}
	putc('\n', stderr);
}

void
csireset(void)
{
	memset(&csiescseq, 0, sizeof(csiescseq));
}

void
strhandle(void)
{
	char *p = NULL, *dec;
	int j, narg, par;

	term.esc &= ~(ESC_STR_END|ESC_STR);
	strparse();
	par = (narg = strescseq.narg) ? atoi(strescseq.args[0]) : 0;

	switch (strescseq.type) {
	case ']': /* OSC -- Operating System Command */
		switch (par) {
		case 0:
		case 1:
		case 2:
			if (narg > 1)
				xsettitle(strescseq.args[1]);
			return;
		case 52:
			if (narg > 2) {
				dec = base64dec(strescseq.args[2]);
				if (dec) {
					xsetsel(dec);
					xclipcopy();
				} else {
					fprintf(stderr, "erresc: invalid base64\n");
				}
			}
			return;
		case 4: /* color set */
			if (narg < 3)
				break;
			p = strescseq.args[2];
			/* FALLTHROUGH */
		case 104: /* color reset, here p = NULL */
			j = (narg > 1) ? atoi(strescseq.args[1]) : -1;
			if (xsetcolorname(j, p)) {
				if (par == 104 && narg <= 1)
					return; /* color reset without parameter */
				fprintf(stderr, "erresc: invalid color j=%d, p=%s\n",
				        j, p ? p : "(null)");
			} else {
				/*
				 * TODO if defaultbg color is changed, borders
				 * are dirty
				 */
				redraw();
			}
			return;
		}
		break;
	case 'k': /* old title set compatibility */
		xsettitle(strescseq.args[0]);
		return;
	case 'P': /* DCS -- Device Control String */
		term.mode |= ESC_DCS;
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
		return;
	}

	fprintf(stderr, "erresc: unknown str ");
	strdump();
}

void
strparse(void)
{
	int c;
	char *p = strescseq.buf;

	strescseq.narg = 0;
	strescseq.buf[strescseq.len] = '\0';

	if (*p == '\0')
		return;

	while (strescseq.narg < STR_ARG_SIZ) {
		strescseq.args[strescseq.narg++] = p;
		while ((c = *p) != ';' && c != '\0')
			++p;
		if (c == '\0')
			return;
		*p++ = '\0';
	}
}

void 
strdump(void) {
	int i;
	uint c;

	fprintf(stderr, "ESC%c", strescseq.type);
	for (i = 0; i < strescseq.len; i++) {
		c = strescseq.buf[i] & 0xff;
		if (c == '\0') {
			putc('\n', stderr);
			return;
		} else if (isprint(c)) {
			putc(c, stderr);
		} else if (c == '\n') {
			fprintf(stderr, "(\\n)");
		} else if (c == '\r') {
			fprintf(stderr, "(\\r)");
		} else if (c == 0x1b) {
			fprintf(stderr, "(\\e)");
		} else {
			fprintf(stderr, "(%02x)", c);
		}
	}
	fprintf(stderr, "ESC\\\n");
}

void 
strreset(void) {
	memset(&strescseq, 0, sizeof(strescseq));
}

void 
kpressNormalMode(char ksym) {
	switch(ksym) {
	case 'p': 
		printf("%d, %d, %d, %d\n", term.cNorm.x, 
				term.cNorm.y, 
				term.cNorm.screenOffset.x, 
				term.cNorm.screenOffset.y);
		break;

	case 'i': normalMode(NULL); /* fallthrough */
	case 'r': redraw(); break;
	case 'j': tmoveto(&term.cNorm, term.cNorm.x, 
				term.cNorm.y + 1, true); 
		break;
	case 'k':
		tmoveto(&term.cNorm, term.cNorm.x, 
				term.cNorm.y - 1, true);
		break;
	case 'l':
		tmoveto(&term.cNorm, term.cNorm.x + 1, 
				term.cNorm.y, true);
		break;
	case 'h':
		tmoveto(&term.cNorm, term.cNorm.x - 1, 
				term.cNorm.y, true);
		break;
	case 'J':
	{
		int const tr = isSet(MODE_ALTSCREEN)?term.screenSize.y - 1:term.c.y;
		int trb = tr - term.screenSize.y + 1;
		int const bSize = getHeight();
		int const d=term.cNorm.y-term.cNorm.screenOffset.y;

		if (between(term.cNorm.screenOffset.y, trb, tr) 
				|| term.cNorm.screenOffset.y >= trb + bSize) {
			for (; trb < 0; trb += bSize);
			term.cNorm.screenOffset.y=trb;
			term.cNorm.y = tr;

		} else {
			term.cNorm.y= (term.cNorm.screenOffset.y
					+=term.screenSize.y) + d;
			term.cNorm.y %= bSize;
			term.cNorm.screenOffset.y %= bSize;
		}
		redraw();
		break;

	}
	case 'K':
	{
		int const bh = getHeight();
		int const lp= ((isSet(MODE_ALTSCREEN) ? term.screenSize.y 
					: term.c.y) + 1) % bh;
		int const bf = mod(lp - 1, bh);
		int const af = mod(bf - term.screenSize.y + 1, bh);
		int const d = term.cNorm.y - term.cNorm.screenOffset.y;
		// Shift position + offset down
		term.cNorm.screenOffset.y = mod(term.cNorm.screenOffset.y 
				- term.screenSize.y, bh);
		term.cNorm.y = (term.cNorm.screenOffset.y + d) % bh;
		// If exceeded the desired area, correct.
		if (modBetween(term.cNorm.screenOffset.y, af, bf, bh)) {
			term.cNorm.screenOffset.y = lp;
		}
		if (modBetween(term.cNorm.y, af, bf, bh)) { term.cNorm.y = lp; }
		redraw();
		break;
	}
	}

}

void 
sendbreak(const Arg *arg) {
	if (tcsendbreak(cmdfd, 0))
		perror("Error sending break");
}

void 
tprinter(char *s, size_t len) {
	if (iofd != -1 && xwrite(iofd, s, len) < 0) {
		perror("Error writing to output file");
		close(iofd);
		iofd = -1;
	}
}

void 
toggleprinter(const Arg *arg) { term.mode ^= MODE_PRINT; }

void 
printscreen(const Arg *arg) { tdump(); }

void 
printsel(const Arg *arg) { tdumpsel(); }

void 
tdumpsel(void) {
	char *ptr;
	if ((ptr = getsel())) {
		tprinter(ptr, strlen(ptr));
		free(ptr);
	}
}

void 
tdumpline(int lineGlobal) {
	char buf[UTF_SIZ];
	Glyph *bp, *end;

	bp = getLine(lineGlobal); // character 0.
	end = &bp[MIN(tlinelen(lineGlobal), term.screenSize.x) - 1];
	if (bp != end || bp->u != ' ') {
		for ( ;bp <= end; ++bp)
			tprinter(buf, utf8encode(bp->u, buf));
	}
	tprinter("\n", 1);
}

void 
tdump(void) {
	int const maxLine = term.screenSize.y + term.c.screenOffset.y;
	for (int i = term.c.screenOffset.y; i < maxLine; ++i) { tdumpline(i); }
}

void 
tputtab(int n) {
	int x = term.c.x - term.c.screenOffset.x;
	int const width = term.screenSize.x - term.c.screenOffset.x;
	// XXX: I think {in/de}crementing x twice in between iterations is not correct here. 
	if (n > 0) {
		while (x < width && n--) for (++x; x < width && !term.tabs[x]; ++x);
	} else if (n < 0) {
		while (x > 0 && n++) for (--x; x > 0 && !term.tabs[x]; --x);
	}
	term.c.x = (LIMIT(x, 0, width - 1)) + term.c.screenOffset.x;
}

void 
tdefutf8(char ascii) {
	if (ascii == 'G')
		term.mode |= MODE_UTF8;
	else if (ascii == '@')
		term.mode &= ~MODE_UTF8;
}

void 
tdeftran(char ascii) {
	static char cs[] = "0B";
	static int vcs[] = {CS_GRAPHIC0, CS_USA};
	char *p;

	if ((p = strchr(cs, ascii)) == NULL) {
		fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
	} else {
		term.trantbl[term.icharset] = vcs[p - cs];
	}
}

void 
tdectest(char c) {
	if (c == '8') { /* DEC screen alignment test. */
		int const x = term.c.x; 
		int const y = term.c.y;
		int const height = min(term.c.screenOffset.
				y + term.screenSize.y , getHeight());
		for (term.c.y = term.c.screenOffset.y; term.c.y < height; 
				++term.c.y) { 
			int const w = min(term.c.screenOffset.x + 
					term.screenSize.x , getWidth(term.c.y));
			for (term.c.x = term.c.screenOffset.x; term.c.x < w; 
					++term.c.x) {
				tsetchar('E');//, &term.c.attr, x, y); 
			}
		}
		term.c.x = x;
		term.c.y = y;
	}
}

void
tstrsequence(uchar c)
{
	strreset();

	switch (c) {
	case 0x90:   /* DCS -- Device Control String */
		c = 'P';
		term.esc |= ESC_DCS;
		break;
	case 0x9f:   /* APC -- Application Program Command */
		c = '_';
		break;
	case 0x9e:   /* PM -- Privacy Message */
		c = '^';
		break;
	case 0x9d:   /* OSC -- Operating System Command */
		c = ']';
		break;
	}
	strescseq.type = c;
	term.esc |= ESC_STR;
}

void
tcontrolcode(uchar ascii)
{
	switch (ascii) {
	case '\t':   /* HT */
		tputtab(1);
		return;
	case '\b':   /* BS */
		tmoveto(&term.c, term.c.x - 1, term.c.y, false);
		return;
	case '\r':   /* CR */
		tmoveto(&term.c, 0, term.c.y, false);
		return;
	case '\f':   /* LF */
	case '\v':   /* VT */
	case '\n':   /* LF */
		/* go to first col if the mode is set */
		tnewline(IS_SET(MODE_CRLF));
		return;
	case '\a':   /* BEL */
		if (term.esc & ESC_STR_END) {
			/* backwards compatibility to xterm */
			strhandle();
		} else {
			xbell();
		}
		break;
	case '\033': /* ESC */
		csireset();
		term.esc &= ~(ESC_CSI|ESC_ALTCHARSET|ESC_TEST);
		term.esc |= ESC_START;
		return;
	case '\016': /* SO (LS1 -- Locking shift 1) */
	case '\017': /* SI (LS0 -- Locking shift 0) */
		term.charset = 1 - (ascii - '\016');
		return;
	case '\032': /* SUB */
		tsetchar('?');//, &term.c.attr, term.c.x, term.c.y);
	case '\030': /* CAN */
		csireset();
		break;
	case '\005': /* ENQ (IGNORED) */
	case '\000': /* NUL (IGNORED) */
	case '\021': /* XON (IGNORED) */
	case '\023': /* XOFF (IGNORED) */
	case 0177:   /* DEL (IGNORED) */
		return;
	case 0x80:   /* TODO: PAD */
	case 0x81:   /* TODO: HOP */
	case 0x82:   /* TODO: BPH */
	case 0x83:   /* TODO: NBH */
	case 0x84:   /* TODO: IND */
		break;
	case 0x85:   /* NEL -- Next line */
		tnewline(1); /* always go to first col */
		break;
	case 0x86:   /* TODO: SSA */
	case 0x87:   /* TODO: ESA */
		break;
	case 0x88:   /* HTS -- Horizontal tab stop */
		assertInRegion(term.c.x, 0);
		term.tabs[term.c.x] = 1;
		break;
	case 0x89:   /* TODO: HTJ */
	case 0x8a:   /* TODO: VTS */
	case 0x8b:   /* TODO: PLD */
	case 0x8c:   /* TODO: PLU */
	case 0x8d:   /* TODO: RI */
	case 0x8e:   /* TODO: SS2 */
	case 0x8f:   /* TODO: SS3 */
	case 0x91:   /* TODO: PU1 */
	case 0x92:   /* TODO: PU2 */
	case 0x93:   /* TODO: STS */
	case 0x94:   /* TODO: CCH */
	case 0x95:   /* TODO: MW */
	case 0x96:   /* TODO: SPA */
	case 0x97:   /* TODO: EPA */
	case 0x98:   /* TODO: SOS */
	case 0x99:   /* TODO: SGCI */
		break;
	case 0x9a:   /* DECID -- Identify Terminal */
		ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 0x9b:   /* TODO: CSI */
	case 0x9c:   /* TODO: ST */
		break;
	case 0x90:   /* DCS -- Device Control String */
	case 0x9d:   /* OSC -- Operating System Command */
	case 0x9e:   /* PM -- Privacy Message */
	case 0x9f:   /* APC -- Application Program Command */
		tstrsequence(ascii);
		return;
	}
	/* only CAN, SUB, \a and C1 chars interrupt a sequence */
	term.esc &= ~(ESC_STR_END|ESC_STR);
}

/*
 * returns 1 when the sequence is finished and it hasn't to read
 * more characters for this sequence, otherwise 0
 */
int
eschandle(uchar ascii)
{
	switch (ascii) {
	case '[':
		term.esc |= ESC_CSI;
		return 0;
	case '#':
		term.esc |= ESC_TEST;
		return 0;
	case '%':
		term.esc |= ESC_UTF8;
		return 0;
	case 'P': /* DCS -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
	case ']': /* OSC -- Operating System Command */
	case 'k': /* old title set compatibility */
		tstrsequence(ascii);
		return 0;
	case 'n': /* LS2 -- Locking shift 2 */
	case 'o': /* LS3 -- Locking shift 3 */
		term.charset = 2 + (ascii - 'n');
		break;
	case '(': /* GZD4 -- set primary charset G0 */
	case ')': /* G1D4 -- set secondary charset G1 */
	case '*': /* G2D4 -- set tertiary charset G2 */
	case '+': /* G3D4 -- set quaternary charset G3 */
		term.icharset = ascii - '(';
		term.esc |= ESC_ALTCHARSET;
		return 0;
	case 'D': /* IND -- Linefeed (deprecated?) */
		//if (term.c.y == term.bot) { tscrollup(term.top, 1); 
		//} else { tmoveto(&term.c, term.c.x, term.c.y + 1, false); }
		tmoveto(&term.c, term.c.x, term.c.y + 1, true);
		break;
	case 'E': /* NEL -- Next line */
		tnewline(1); /* always go to first col */
		break;
	case 'H': /* HTS -- Horizontal tab stop */
		assertInRegion(term.c.x, 0);
		term.tabs[term.c.x] = 1;
		break;
	case 'M': /* RI -- Reverse index */
		//if (term.c.y - term.c.screenOffset.y == term.top) { tscrolldown(&term.c, term.c.y, 1); 
		//} else { //	tmoveto(&term.c, term.c.x, term.c.y - 1, false); //}
		tmoveto(&term.c, term.c.x, term.c.y - 1, true); // experimental replacement
		break;
	case 'Z': /* DECID -- Identify Terminal */
		ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 'c': /* RIS -- Reset to initial state */
		treset();
		resettitle();
		xloadcols();
		break;
	case '=': /* DECPAM -- Application keypad */
		xsetmode(1, MODE_APPKEYPAD);
		break;
	case '>': /* DECPNM -- Normal keypad */
		xsetmode(0, MODE_APPKEYPAD);
		break;
	case '7': /* DECSC -- Save Cursor */
		tcursor(CURSOR_SAVE);
		break;
	case '8': /* DECRC -- Restore Cursor */
		tcursor(CURSOR_LOAD);
		break;
	case '\\': /* ST -- String Terminator */
		if (term.esc & ESC_STR_END)
			strhandle();
		break;
	default:
		fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
			(uchar) ascii, isprint(ascii)? ascii:'.');
		break;
	}
	return 1;
}

void
tputc(Rune u)
{
	char c[UTF_SIZ];
	int control;
	int width, len;
	Glyph *gp;

	control = ISCONTROL(u);
	if (!IS_SET(MODE_UTF8) && !IS_SET(MODE_SIXEL)) {
		c[0] = u;
		width = len = 1;
	} else {
		len = utf8encode(u, c);
		if (!control && (width = wcwidth(u)) == -1) {
			memcpy(c, "\357\277\275", 4); /* UTF_INVALID */
			width = 1;
		}
	}

	if (IS_SET(MODE_PRINT))
		tprinter(c, len);

	/*
	 * STR sequence must be checked before anything else
	 * because it uses all following characters until it
	 * receives a ESC, a SUB, a ST or any other C1 control
	 * character.
	 */
	if (term.esc & ESC_STR) {
		if (u == '\a' || u == 030 || u == 032 || u == 033 ||
		   ISCONTROLC1(u)) {
			term.esc &= ~(ESC_START|ESC_STR|ESC_DCS);
			if (IS_SET(MODE_SIXEL)) {
				/* TODO: render sixel */;
				term.mode &= ~MODE_SIXEL;
				return;
			}
			term.esc |= ESC_STR_END;
			goto check_control_code;
		}

		if (IS_SET(MODE_SIXEL)) {
			/* TODO: implement sixel mode */
			return;
		}
		if (term.esc&ESC_DCS && strescseq.len == 0 && u == 'q')
			term.mode |= MODE_SIXEL;

		if (strescseq.len+len >= sizeof(strescseq.buf)-1) {
			/*
			 * Here is a bug in terminals. If the user never sends
			 * some code to stop the str or esc command, then st
			 * will stop responding. But this is better than
			 * silently failing with unknown characters. At least
			 * then users will report back.
			 *
			 * In the case users ever get fixed, here is the code:
			 */
			/*
			 * term.esc = 0;
			 * strhandle();
			 */
			return;
		}

		memmove(&strescseq.buf[strescseq.len], c, len);
		strescseq.len += len;
		return;
	}

check_control_code:
	/*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
	if (control) {
		tcontrolcode(u);
		/*
		 * control codes are not shown ever
		 */
		return;
	} else if (term.esc & ESC_START) {
		if (term.esc & ESC_CSI) {
			csiescseq.buf[csiescseq.len++] = u;
			if (BETWEEN(u, 0x40, 0x7E)
					|| csiescseq.len >= \
					sizeof(csiescseq.buf)-1) {
				term.esc = 0;
				csiparse();
				csihandle();
			}
			return;
		} else if (term.esc & ESC_UTF8) {
			tdefutf8(u);
		} else if (term.esc & ESC_ALTCHARSET) {
			tdeftran(u);
		} else if (term.esc & ESC_TEST) {
			tdectest(u);
		} else {
			if (!eschandle(u))
				return;
			/* sequence already finished */
		}
		term.esc = 0;
		/*
		 * All characters which form part of a sequence are not
		 * printed
		 */
		return;
	}
	if (sel.ob.x != -1 && BETWEEN(term.c.y, sel.ob.y, sel.oe.y))
		selclear();

	gp = &(getLine(term.c.y)[term.c.x]);
	if (IS_SET(MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
		gp->mode |= ATTR_WRAP;
		tnewline(1);
		gp = &(getLine(term.c.y)[term.c.x]);
	}

	if (IS_SET(MODE_INSERT) && term.c.x + width < term.screenSize.x)
		memmove(gp+width, gp, (term.screenSize.x - term.c.x - width) * sizeof(Glyph));

	if (term.c.x+width > term.screenSize.x) {
		tnewline(1);
		gp = &(getLine(term.c.y)[term.c.x]);
	}

	tsetchar(u);//, &term.c.attr, term.c.x, term.c.y);

	if (width == 2) {
		gp->mode |= ATTR_WIDE;
		if (term.c.x+1 < term.screenSize.x) {
			gp[1].u = '\0';
			gp[1].mode = ATTR_WDUMMY;
		}
	}
	if (term.c.x+width < term.screenSize.x) {
		tmoveto(&term.c, term.c.x + width, term.c.y, false);
	} else {
		term.c.state |= CURSOR_WRAPNEXT;
	}
}

int twrite(const char *buf, int buflen, int show_ctrl) {
	int charsize;
	Rune u;
	int n;

	for (n = 0; n < buflen; n += charsize) {
		if (IS_SET(MODE_UTF8) && !IS_SET(MODE_SIXEL)) {
			/* process a complete utf8 char */
			charsize = utf8decode(buf + n, &u, buflen - n);
			if (charsize == 0)
				break;
		} else {
			u = buf[n] & 0xFF;
			charsize = 1;
		}
		if (show_ctrl && ISCONTROL(u)) {
			if (u & 0x80) {
				u &= 0x7f;
				tputc('^');
				tputc('[');
			} else if (u != '\n' && u != '\r' && u != '\t') {
				u ^= 0x40;
				tputc('^');
			}
		}
		tputc(u);
	}
	return n;
}

void 
tresize(int newWidthScreen, int newHeightScreen) {
	LIMIT(newHeightScreen, 0, maxAmountRows);
	if (newWidthScreen < 1 || newHeightScreen < 1) {
		fprintf(stderr, "tresize: error resizing to %dx%d\n", newWidthScreen, newHeightScreen);
		return;
	} else if (newWidthScreen == term.screenSize.x && newHeightScreen == term.screenSize.y) { 
		return; 
	}
	Position const oldScreenSize = term.screenSize;
	Position const oldTotalSize = term.totalSize;
	// Apply new screen size
	bool const alt = isSet(MODE_ALTSCREEN);
	if (alt) { term.mode ^= MODE_ALTSCREEN; }
	size_t const rowSizeBytesAlt = newWidthScreen * sizeof(Glyph);

	// Update the existing rows to the new amount of columns in the buffer 
	// and alternate buffer. In case the terminal becomes larger, resize 
	// each row. Otherwise keep the rows as-is;
	if (newWidthScreen > oldTotalSize.x) {
		for (int row = 0; row < oldTotalSize.y; ++row) {
			term.buffer[row] = xrealloc(term.buffer[row], rowSizeBytesAlt);// rowSizeAlt = rowSize here.
		}
		term.totalSize.x = newWidthScreen;
	} // Update alt screen columns
	if (newWidthScreen != oldScreenSize.x) {
		for (int row = 0; row < oldScreenSize.y; ++row) {
			term.alt[row] = xrealloc(term.alt[row], rowSizeBytesAlt);
		}
		term.screenSize.x = newWidthScreen;
	} //< updated term.{total, screen}Size.x until now
	
	// Update the amount of rows in the buffer and alternate buffer.
	if (newHeightScreen > oldTotalSize.y) {
		size_t const rowSizeBytes = term.totalSize.x * sizeof(Glyph);
		for (; term.totalSize.y < newHeightScreen; ++term.totalSize.y) {
			term.buffer[term.totalSize.y] = xmalloc(rowSizeBytes);
		}
	} // Alt screen
	if (newHeightScreen != oldScreenSize.y) {
		term.alt = xrealloc(term.alt, newHeightScreen * sizeof(Line));
		for (int row = oldScreenSize.y; row < newHeightScreen; ++row) {
			term.alt[row] = xmalloc(rowSizeBytesAlt);
		}
		term.screenSize.y = newHeightScreen;
	} //<  updated term.{total, screen}Size.y until now

	term.dirty = xrealloc(term.dirty, term.screenSize.y * sizeof(*term.dirty));
	term.tabs  = xrealloc(term.tabs,  term.totalSize.x  * sizeof(*term.tabs));
	if (term.totalSize.x > oldTotalSize.x) {
		int* bp = term.tabs + oldTotalSize.x;
		memset(bp, 0, sizeof(*term.tabs) * (term.totalSize.x - oldTotalSize.x));
		while (--bp > term.tabs && !*bp) /* nothing */ ;
		for (bp += tabspaces; bp < term.tabs + term.totalSize.x; bp += tabspaces) { *bp = 1; }
	}

	// Clear the regions for the ALT and the normal buffer for growth in cols and rows.
	// If no change occurs, the function exist immediately.
	tclearregion(oldTotalSize.x, 0, newWidthScreen - 1, oldTotalSize.y - 1);
	tclearregion(0, oldTotalSize.y, term.totalSize.x - 1, newHeightScreen - 1);
	term.mode ^= MODE_ALTSCREEN; 
	tclearregion(oldScreenSize.x, 0, newWidthScreen - 1, oldScreenSize.y - 1);
	tclearregion(0, oldScreenSize.y, term.screenSize.x - 1, newHeightScreen - 1);
	if (!alt) { term.mode ^= MODE_ALTSCREEN; }

	// Reset the current screen position if the screen size is reduced; repaint the changed regions
	TCursor *activeCursor = isInsertCursor() ? &term.c : &term.cNorm;
	if (newHeightScreen < oldScreenSize.y) {
		if (activeCursor->y >= activeCursor->screenOffset.y + newHeightScreen) { //< move up
			activeCursor->screenOffset.y = activeCursor->y - newHeightScreen - 1;
			tfulldirt();
		}
	}
	// reset top and bot
	tsetscroll(0, term.screenSize.y-1);
	/* make use of the LIMIT in tmoveto */
	tmoveto(activeCursor, activeCursor->x, activeCursor->y, false); // not required i think.
	TCursor c = term.c;

	// Load both cursors
	tswapscreen();
	tcursor(CURSOR_LOAD);
	tcursor(CURSOR_SAVE);

	tswapscreen();
	tcursor(CURSOR_LOAD);
	tcursor(CURSOR_SAVE);
	
	term.c = c;
}

void 
resettitle(void) { xsettitle(NULL); }

void 
drawregion(int x1, int y1, int x2, int y2, Position offset) {
	assertInScreen(x1, y1);
	assertInScreen(x2, y2);
	int y;
	for (y = y1; y <= y2; y++) {
		if (term.dirty[y]) {
			term.dirty[y] = 0;
			xdrawline(getLine(y + offset.y) + offset.x, x1, y, x2+1);
		}
	}
}

void 
draw(void) {
	if (!xstartdraw()) return;

	TCursor const *activeCursor = isInsertCursor() ? &term.c : &term.cNorm;
	// The current cursor position is guaranteed to be in region.
	assertInRegion(activeCursor->x, activeCursor->y);
	int cx = activeCursor->x;
	// The old cursor position is required for the drawcursor function, hence 
	// it is not reset if the screen is resized and everything is repainted.
	LIMIT(term.oldCursor.x, 0, term.screenSize.x-1);
	LIMIT(term.oldCursor.y, 0, term.screenSize.y-1);
	// If the cursor is behind, move one letter back
	Glyph *preGlyph = getLine(term.oldCursor.y + activeCursor->screenOffset.y)
		+ term.oldCursor.x + activeCursor->screenOffset.x;
	if (preGlyph->mode & ATTR_WDUMMY) { --term.oldCursor.x; }
	if (getLine(activeCursor->y)[cx].mode & ATTR_WDUMMY) { --cx; }
	

	int const rcx = cx - activeCursor->screenOffset.x;
	int rcy = activeCursor->y - activeCursor->screenOffset.y;
	for (;rcy < 0; rcy += getHeight());
	assertInScreen(rcx, rcy);
	drawregion(0, 0, term.screenSize.x - 1, 
			term.screenSize.y - 1, activeCursor->screenOffset);
	xdrawcursor(rcx, rcy, getLine(activeCursor->y)[cx], term.oldCursor.x, 
			        term.oldCursor.y, *preGlyph);


	term.oldCursor.x = rcx;
	term.oldCursor.y = rcy;
	xfinishdraw();
	xximspot(term.oldCursor.x, term.oldCursor.y);
}

void 
redraw(void) {
	tfulldirt();
	draw();
}
