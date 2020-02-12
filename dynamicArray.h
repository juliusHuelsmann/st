#ifndef DYNAMIC_ARRAY_H
#define DYNAMIC_ARRAY_H

#include "error.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/// Struct for which this file offers functionality in order to expand the array
/// and set / get its content.
typedef struct DynamicArray {
	/// Size of the datatype contained in the array.
	uint8_t itemSize;
	/// Amount of bytes currently initialized
	uint32_t index;
	/// Amount of bytes currently reserved (not necessarily initialized)
	uint32_t allocated;
	/// Actual content.
	char* content;
} DynamicArray;

#define EXPAND_STEP 15

/// Default initializers for the dynamic array.
#define CHAR_ARRAY  {1, 0, 0, NULL}
#define WORD_ARRAY  {2, 0, 0, NULL}
#define DWORD_ARRAY {4, 0, 0, NULL}
#define QWORD_ARRAY {8, 0, 0, NULL}
/// (Wasteful) utf-8 array, that always used 4 bytes in order to display a
/// character, even if the space is not required.
#define UTF8_ARRAY  DWORD_ARRAY

/// Check that at least \p bytes are allocated, if true implying that
/// \p s->content[\bytes - 1] is allocated.
static inline bool
isAllocated(DynamicArray const *s, uint32_t bytes) {
	return s != NULL && s->allocated >= bytes;
}

/// @see #isAllocated
static inline bool
isInitialized(DynamicArray const *s, uint32_t bytes) {
	return s != NULL && s->index >= bytes;
}

/// Return the next element in \p s and increment index without checking bounds.
static inline char*
gnext(DynamicArray *s) {
	ENSURE(s!=NULL, return NULL);
	ENSURE(s->index % s->itemSize == 0 && "(index not aligned)",
			s->index += s->itemSize - (s->index % s->itemSize));
	ENSURE(isAllocated(s, s->index + 2 * s->itemSize), return NULL);
	return s->content + (s->index += s->itemSize);
}

/// View element \p i in \p s.
static inline char*
view(DynamicArray const * s, uint32_t i) {
	ENSURE((s != NULL) && isAllocated(s, (i+1) * s->itemSize), return NULL);
	return s->content + i*s->itemSize;
}

/// Inspect element content[size() - 1 - i].
static inline char *
viewEnd(DynamicArray const *s, uint32_t i) {
	ENSURE((s != NULL) && isInitialized(s, i * s->itemSize), return NULL);
	ENSURE(s->index%s->itemSize == 0 && "(index not aligned)", return NULL);
	return s->content + s->index - (i + 1) * s->itemSize;
}

/// Set conent without applying
static inline bool
setValues(DynamicArray* s, char const *vals, uint32_t amount) {
	ENSURE(vals != NULL, return false);
	ENSURE((s != NULL) && isAllocated(s, s->index + amount), return false);
	memcpy(s->content + s->index, vals, amount);
	return true;
}

static inline bool
snext(DynamicArray* s, char const *vals, uint32_t amount) {
	bool const success = setValues(s, vals, amount);
	ENSURE(success, return false);
	uint8_t const rest = amount % s->itemSize;
	uint32_t const newSize = s->index + amount + (rest ? s->itemSize : 0);
	ENSURE(isAllocated(s, newSize), return false);
	s->index = newSize;
	return true;
}

/// Empty \p s.
static inline void
empty(DynamicArray* s) {
	ENSURE((s != NULL), return);
	s->index = 0;
}

/// Check if \p s has initialized content (which can be the case even if memory
/// is allocated).
static inline bool
isEmpty(DynamicArray const * s) {
	ENSURE((s != NULL), return true);
	return s->index == 0;
}

static inline int
size(DynamicArray const * s) {
	ENSURE(s != NULL, return 0);
	ENSURE(s->itemSize != 0, return 0);
	return s->index / s->itemSize;
}

static inline void
pop(DynamicArray* s) {
	ENSURE((s != NULL), return);
	ENSURE(s->index % s->itemSize == 0 && "(index not aligned)",
			s->index += s->itemSize - (s->index % s->itemSize));
	ENSURE(isInitialized(s, s->itemSize), return);
	s->index -= s->itemSize;
}

static inline bool
checkSetNext(DynamicArray *s, char const *c, uint32_t amount) {
	ENSURE(s != NULL && c != NULL, return false);
	if (s->allocated < s->index + s->itemSize * amount) {
		uint32_t const diff = s->index+s->itemSize*amount-s->allocated;
		uint32_t const newAlloSize = s->allocated + (diff > EXPAND_STEP
				? diff : EXPAND_STEP) * s->itemSize;
		char* tmp = realloc(s->content, newAlloSize);
		if (tmp == NULL) { return false; }
		s->allocated = newAlloSize;
		s->content = tmp;
		assert(s->allocated >= s->index + s->itemSize * amount);
	}
	if (amount) { snext(s, c, amount); }
	return true;
}

static inline bool
checkSetNextV(DynamicArray *s, char const c) {
	return checkSetNext(s, &c, 1);
}

static inline bool
checkSetNextP(DynamicArray *s, char const *c) {
	ENSURE(c != NULL, return false);
	return checkSetNext(s, c, strlen(c));
}

/// Expand the currently initialized content in \p s and the allocated chunk of
/// memory if required.
static char *
expand(DynamicArray *s) {
	ENSURE(s != NULL, return NULL);
	if (s->allocated < s->index + s->itemSize) {
		uint32_t const diff = s->index + s->itemSize - s->allocated;
		uint32_t const newAlloSize = s->allocated + (diff > EXPAND_STEP
				? diff : EXPAND_STEP) * s->itemSize;
		char* tmp = realloc(s->content, newAlloSize);
		if (tmp == NULL) { return NULL; }
		s->allocated = newAlloSize;
		s->content = tmp;
		assert(s->allocated >= s->index + s->itemSize);
	}
	s->index+=s->itemSize;
	return viewEnd(s, 0);
}

#define append(s, c) checkSetNext((s), (char const *) (c), (s)->itemSize)
#define appendPartial(s, c, i) checkSetNext((s), (char const *) (c), (i))


#endif // DYNAMIC_ARRAY_H
