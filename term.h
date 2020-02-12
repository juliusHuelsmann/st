#ifndef TERM_H
#define TERM_H

//
// Internal terminal structs.
//

#include "glyph.h"

#include <stdint.h>

#define HISTSIZE      2500

typedef struct {
	Glyph attr; /* current char attributes */
	int x;
	int y;
	char state;
} TCursor;

typedef struct {
	int mode;
	int type;
	int snap;
	/// Selection variables:
	/// ob – original coordinates of the beginning of the selection
	/// oe – original coordinates of the end of the selection
	struct {
		int x, y, scroll;
	} ob, oe;
	/// Selection variables; currently displayed chunk.
	/// nb – normalized coordinates of the beginning of the selection
	/// ne – normalized coordinates of the end of the selection
	struct {
		int x, y;
	} nb, ne;

	int alt;
} Selection;

/* Internal representation of the screen */
typedef struct {
	int row;      /* nb row */
	int col;      /* nb col */
	Line *line;   /* screen */
	Line *alt;    /* alternate screen */
	Line hist[HISTSIZE]; /* history buffer */
	int histi;    /* history index */
	int scr;      /* scroll back */
	int *dirty;   /* dirtyness of lines */
	TCursor c;    /* cursor */
	int ocx;      /* old cursor col */
	int ocy;      /* old cursor row */
	int top;      /* top    scroll limit */
	int bot;      /* bottom scroll limit */
	int mode;     /* terminal mode flags */
	int esc;      /* escape state flags */
	char trantbl[4]; /* charset table translation */
	int charset;  /* current charset */
	int icharset; /* selected charset for sequence */
	int *tabs;
} Term;

extern Term term;

#define TLINE(y) ((y) < term.scr ? term.hist[((y) + term.histi - \
		 term.scr + HISTSIZE + 1) % HISTSIZE] : \
		 term.line[(y) - term.scr])

extern Selection sel;


#endif // TERM_H
