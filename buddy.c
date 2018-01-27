/* Copyright (c) 2018, Saggi Mizrahi
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Buddy allocator
 *
 * notes:
 * This module uses 2 bits per md slot. The reason for all this complicated
 * bit gymnastics is to use as little space as possible from the memory pool
 * for overhead.
 */

#include "buddy.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#define NODE_UNUSED 0
#define NODE_USED   1
#define NODE_SPLIT  2
#define NODE_FULL   3

struct buddy {
	unsigned int              min_level;
	unsigned int              level;
	buddy_buffer_destructor_t destructor;
	uint8_t                   tree[];
};

struct buddy * buddy_from_buffer(unsigned int              level,
                                 unsigned int              min_level,
                                 void *                    buffer,
                                 buddy_buffer_destructor_t destructor
) {
	struct buddy *self = (struct buddy *) buffer;
	self->destructor = destructor;
	self->level = level - min_level;
	if (self->level < 2 || min_level < 4) {
		return NULL;
	}

	self->min_level = min_level;
	int md_size = 1 << (self->level - 1);
	memset(self->tree, 0, md_size);
	// reserve md space
	buddy_alloc(self, md_size);
	return self;
}

struct buddy * buddy_new(unsigned int level, unsigned int min_level)
{
	void *buff = malloc(1 << level);
	if (!buff) {
		return NULL;
	}

	return buddy_from_buffer(level, min_level, buff, free);
}

void buddy_destroy(struct buddy * self)
{
	if (self->destructor) {
		self->destructor(self);
	}
}

static inline int _get_index_status(struct buddy * self, int index)
{
	return (self->tree[index >> 2] >> ((index & 3) * 2)) & 3;
}

static inline void _set_index_status(struct buddy * self,
                                     int index,
                                     int status
) {
	//@tbd: there might be a more optimized way to do it
	uint8_t old = self->tree[index >> 2] & ~(3 << ((index & 3) * 2));
	self->tree[index >> 2] = old | (status << ((index & 3) * 2));
}

static inline int is_pow_of_2(uint32_t x)
{
	return !(x & (x-1));
}

static inline uint32_t next_pow_of_2(uint32_t x)
{
	if ( is_pow_of_2(x) )
	{
		return x;
	}
#if defined(__GNUC__)
	return 0x80000000 >> (__builtin_clzl(x) - 1);
#else
	x |= x>>1;
	x |= x>>2;
	x |= x>>4;
	x |= x>>8;
	x |= x>>16;

	return x+1;
#endif
}

static inline int _index_offset(
	struct buddy *self,
	int index,
	int level,
	int max_level
) {
	return (((index + 1) - (1 << level)) << (max_level - level)) << self->min_level;
}

static void _mark_parent(struct buddy * self, int index)
{
	for (;;) {
		int buddy = index - 1 + (index & 1) * 2;
		if (buddy > 0 && (_get_index_status(self, buddy) == NODE_USED || _get_index_status(self, buddy) == NODE_FULL)) {
			index = (index + 1) / 2 - 1;
			_set_index_status(self, index, NODE_FULL);
		} else {
			return;
		}
	}
}

void * buddy_alloc(struct buddy * self, size_t s)
{
	int size;
	if (s <= (1 << self->min_level)) {
		size = 1;
	} else {
		size = (int)next_pow_of_2(s) >> self->min_level;
	}

	int length = 1 << self->level;

	if (size > length)
	{
		return NULL;
	}

	int index = 0;
	int level = 0;

	while (index >= 0) {
		if (size == length) {
			if (_get_index_status(self, index) == NODE_UNUSED) {
				_set_index_status(self, index, NODE_USED);
				_mark_parent(self, index);
				return ((char *)self->tree) + _index_offset(self, index, level, self->level);
			}
		} else {
			// size < length
			switch (_get_index_status(self, index)) {
			case NODE_USED:
			case NODE_FULL:
				break;
			case NODE_UNUSED:
				// split first
				_set_index_status(self, index, NODE_SPLIT);
				_set_index_status(self, index * 2 + 1, NODE_UNUSED);
				_set_index_status(self, index * 2 + 2, NODE_UNUSED);
			default:
				index = index * 2 + 1;
				length /= 2;
				level++;
				continue;
			}
		}
		if (index & 1) {
			++index;
			continue;
		}
		for (;;) {
			level--;
			length *= 2;
			index = (index+1)/2 -1;
			if (index < 0)
				return NULL;
			if (index & 1) {
				++index;
				break;
			}
		}
	}

	return NULL;
}

static void _combine(struct buddy * self, int index)
{
	for (;;) {
		int buddy = index - 1 + (index & 1) * 2;
		if (buddy < 0 || _get_index_status(self, buddy) != NODE_UNUSED) {
			_set_index_status(self, index, NODE_UNUSED);
			while (((index = (index + 1) / 2 - 1) >= 0) && _get_index_status(self, index) == NODE_FULL){
				_set_index_status(self, index, NODE_SPLIT);
			}
			return;
		}
		index = (index + 1) / 2 - 1;
	}
}

void buddy_free(struct buddy * self, void *ptr)
{
	int offset = (((uintptr_t) ptr) - ((uintptr_t) self->tree)) >> self->min_level;
	int left = 0;
	int length = 1 << self->level;
	int index = 0;
	assert(offset >= 0 && offset < length);

	for (;;) {
		switch (_get_index_status(self, index)) {
		case NODE_USED:
			assert(offset == left);
			_combine(self, index);
			return;
		case NODE_UNUSED:
			assert(0);
			return;
		default:
			length /= 2;
			if (offset < left + length) {
				index = index * 2 + 1;
			} else {
				left += length;
				index = index * 2 + 2;
			}
			break;
		}
	}
}
