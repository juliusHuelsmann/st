#ifndef LINE_H
#define LINE_H

//
// Contains the representation of the entities in the buffer (Line, Gylph), that
// is used by every part of the software implmeneting terminal logic.
//

#include <stdint.h>

enum selection_type {
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

typedef uint_least32_t Rune;

#define Glyph Glyph_

typedef struct {
	Rune u;           /* character code */
	unsigned short mode;      /* attribute flags */
	uint32_t fg;      /* foreground  */
	uint32_t bg;      /* background  */
} Glyph;


typedef Glyph *Line;

#endif // LINE_H
