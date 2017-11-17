
/**
 * @file gaba.c
 *
 * @brief libgaba (libsea3) DP routine implementation
 *
 * @author Hajime Suzuki
 * @date 2016/1/11
 * @license Apache v2
 */
// #define DEBUG
/* make sure POSIX APIs are properly activated */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE		200112L
#endif

#if defined(__darwin__) && !defined(_BSD_SOURCE)
#  define _BSD_SOURCE
#endif

/* import general headers */
#include <stdio.h>				/* sprintf in dump_path */
#include <stdint.h>				/* uint32_t, uint64_t, ... */
#include <stddef.h>				/* offsetof */
#include <string.h>				/* memset, memcpy */
#include "gaba.h"
#include "log.h"
#include "sassert.h"
#include "arch/arch.h"			/* architecture dependents */


/**
 * gap penalty model configuration: choose one of the following three by a define macro
 *   linear:   g(k) =          ge * k          where              ge > 0
 *   affine:   g(k) =     gi + ge * k          where gi > 0,      ge > 0
 *   combined: g(k) = min(gi + ge * k, gf * k) where gi > 0, gf > ge > 0
 */
#define LINEAR 						1
#define AFFINE						2
#define COMBINED					3

#ifdef MODEL
#  if !(MODEL == LINEAR || MODEL == AFFINE || MODEL == COMBINED)
#    error "MODEL must be LINEAR (1), AFFINE (2), or COMBINED (3)."
#  endif
#else
#  define MODEL 					AFFINE
#endif

#if MODEL == LINEAR
#  define MODEL_LABEL				linear
#elif MODEL == AFFINE
#  define MODEL_LABEL				affine
#else
#  define MODEL_LABEL				combined
#endif

#ifdef BIT
#  if BIT == 2 || BIT == 4
#    error "BIT must be 2 or 4."
#  endif
#else
#  define BIT						4
#endif

/* define ENABLE_FILTER to enable gapless alignment filter */
// #define ENABLE_FILTER


/* bandwidth-specific configurations aliasing vector macros */
#define BW_MAX						64
#ifndef BW
#  define BW						64
#endif

#if BW == 16
#  define _NVEC_ALIAS_PREFIX		v16i8
#  define _WVEC_ALIAS_PREFIX		v16i16
#  define DP_CTX_INDEX				2
#elif BW == 32
#  define _NVEC_ALIAS_PREFIX		v32i8
#  define _WVEC_ALIAS_PREFIX		v32i16
#  define DP_CTX_INDEX				1
#elif BW == 64
#  define _NVEC_ALIAS_PREFIX		v64i8
#  define _WVEC_ALIAS_PREFIX		v64i16
#  define DP_CTX_INDEX				0
#else
#  error "BW must be one of 16, 32, or 64."
#endif
#include "arch/vector_alias.h"

#define DP_CTX_MAX					( 3 )
#define _dp_ctx_index(_bw)			( ((_bw) == 64) ? 0 : (((_bw) == 32) ? 1 : 2) )
_static_assert(_dp_ctx_index(BW) == DP_CTX_INDEX);


/* add suffix for gap-model- and bandwidth-wrapper (see gaba_wrap.h) */
#ifdef SUFFIX
#  define _suffix_cat3_2(a, b, c)	a##_##b##_##c
#  define _suffix_cat3(a, b, c)		_suffix_cat3_2(a, b, c)
#  define _suffix(_base)			_suffix_cat3(_base, MODEL_LABEL, BW)
#else
#  define _suffix(_base)			_base
#endif


/* add namespace for arch wrapper (see main.c) */
#ifdef NAMESPACE
#  define _export_cat(x, y)			x##_##y
#  define _export_cat2(x, y)		_export_cat(x, y)
#  define _export(_base)			_export_cat2(NAMESPACE, _suffix(_base))
#else
#  define _export(_base)			_suffix(_base)
#endif


/* import unittest */
#ifndef UNITTEST_UNIQUE_ID
#  if MODEL == LINEAR
#    if BW == 16
#      define UNITTEST_UNIQUE_ID	31
#    elif BW == 32
#      define UNITTEST_UNIQUE_ID	32
#    else
#      define UNITTEST_UNIQUE_ID	33
#    endif
#  elif MODEL == AFFINE
#    if BW == 16
#      define UNITTEST_UNIQUE_ID	34
#    elif BW == 32
#      define UNITTEST_UNIQUE_ID	35
#    else
#      define UNITTEST_UNIQUE_ID	36
#    endif
#  else
#    if BW == 16
#      define UNITTEST_UNIQUE_ID	37
#    elif BW == 32
#      define UNITTEST_UNIQUE_ID	38
#    else
#      define UNITTEST_UNIQUE_ID	39
#    endif
#  endif
#endif
#include  "unittest.h"


/* internal constants */
#define BLK_BASE					( 5 )
#define BLK 						( 0x01<<BLK_BASE )

#define MIN_BULK_BLOCKS				( 32 )
#define MEM_ALIGN_SIZE				( 32 )		/* 32byte aligned for AVX2 environments */
#define MEM_INIT_SIZE				( (uint64_t)256 * 1024 * 1024 )
#define MEM_MARGIN_SIZE				( 2048 )	/* tail margin of internal memory blocks */
#define GP_INIT						( 1 )		/* global p coordinate where total fetched sequence length becomes BW */
#define GP_ROOT						( -1 )		/* global p coordinate for the first vector of the first block */

/* test consistency of exported macros */
_static_assert(V2I32_MASK_01 == GABA_UPDATE_A);
_static_assert(V2I32_MASK_10 == GABA_UPDATE_B);


/**
 * @macro _likely, _unlikely
 * @brief branch prediction hint for gcc-compatible compilers
 */
#define _likely(x)					__builtin_expect(!!(x), 1)
#define _unlikely(x)				__builtin_expect(!!(x), 0)

/**
 * @macro _force_inline
 * @brief inline directive for gcc-compatible compilers
 */
#define _force_inline				inline
// #define _force_inline				/* */

/** assume 64bit little-endian system */
_static_assert(sizeof(void *) == 8);

/** check size of structs declared in gaba.h */
_static_assert(sizeof(struct gaba_params_s) == 48);
_static_assert(sizeof(struct gaba_section_s) == 16);
_static_assert(sizeof(struct gaba_fill_s) == 24);
_static_assert(sizeof(struct gaba_segment_s) == 32);
_static_assert(sizeof(struct gaba_alignment_s) == 64);
_static_assert(sizeof(nvec_masku_t) == BW / 8);

/**
 * @macro _max_match, _gap_h, _gap_v
 * @brief calculate scores
 */
#define _max_match(_p)				( _hmax_v16i8(_loadu_v16i8((_p)->score_matrix)) )
#define _max_match_base(_p)			( 0x01 )
#if MODEL == LINEAR
#define _gap_h(_p, _l)				( -1 * ((_p)->gi + (_p)->ge) * (_l) )
#define _gap_v(_p, _l)				( -1 * ((_p)->gi + (_p)->ge) * (_l) )
#elif MODEL == AFFINE
#define _gap_h(_p, _l)				( -1 * ((_l) > 0) * (_p)->gi - (_p)->ge * (_l) )
#define _gap_v(_p, _l)				( -1 * ((_l) > 0) * (_p)->gi - (_p)->ge * (_l) )
#else /* MODEL == COMBINED */
#define _gap_h(_p, _l)				( MAX2(-1 * ((_l) > 0) * (_p)->gi - (_p)->ge * (_l), -1 * (_p)->gfa * (_l)) )
#define _gap_v(_p, _l)				( MAX2(-1 * ((_l) > 0) * (_p)->gi - (_p)->ge * (_l), -1 * (_p)->gfb * (_l)) )
#endif
#define _ofs_h(_p)					( (_p)->gi + (_p)->ge )
#define _ofs_v(_p)					( (_p)->gi + (_p)->ge )
#define _ofs_e(_p)					( (_p)->gi )
#define _ofs_f(_p)					( (_p)->gi )

/**
 * @macro _plen
 * @brief extract plen from path_section_s
 */
#define _plen(seg)					( (seg)->alen + (seg)->blen )

/* forward declarations */
static int64_t gaba_dp_add_stack(struct gaba_dp_context_s *self, uint64_t size);
static void *gaba_dp_malloc(struct gaba_dp_context_s *self, uint64_t size);
static void gaba_dp_free(struct gaba_dp_context_s *self, void *ptr);				/* do nothing */
struct gaba_dp_context_s;


/**
 * @struct gaba_small_delta_s
 */
struct gaba_small_delta_s {
	int8_t delta[BW];					/** (32) small delta */
};
_static_assert(sizeof(struct gaba_small_delta_s) == BW);

/**
 * @struct gaba_drop_s
 */
struct gaba_drop_s {
	int8_t drop[BW];					/** (32) max */
};
_static_assert(sizeof(struct gaba_small_delta_s) == BW);

/**
 * @struct gaba_middle_delta_s
 */
struct gaba_middle_delta_s {
	int16_t delta[BW];					/** (64) middle delta */
};
_static_assert(sizeof(struct gaba_middle_delta_s) == sizeof(int16_t) * BW);

/**
 * @struct gaba_mask_pair_u
 */
#if MODEL == LINEAR
struct gaba_mask_pair_s {
	nvec_masku_t h;						/** (4) horizontal mask vector */
	nvec_masku_t v;						/** (4) vertical mask vector */
};
_static_assert(sizeof(struct gaba_mask_pair_s) == BW / 4);
#else	/* affine and combined */
struct gaba_mask_pair_s {
	nvec_masku_t h;						/** (4) horizontal mask vector */
	nvec_masku_t v;						/** (4) vertical mask vector */
	nvec_masku_t e;						/** (4) e mask vector */
	nvec_masku_t f;						/** (4) f mask vector */
};
_static_assert(sizeof(struct gaba_mask_pair_s) == BW / 2);
#endif

/**
 * @struct gaba_diff_vec_s
 */
#if MODEL == LINEAR
struct gaba_diff_vec_s {
	uint8_t dh[BW];						/** (32) dh */
	uint8_t dv[BW];						/** (32) dv */
};
_static_assert(sizeof(struct gaba_diff_vec_s) == 2 * BW);
#else	/* affine and combined gap penalty */
struct gaba_diff_vec_s {
	uint8_t dh[BW];						/** (32) dh */
	uint8_t dv[BW];						/** (32) dv */
	uint8_t de[BW];						/** (32) de */
	uint8_t df[BW];						/** (32) df */
};
_static_assert(sizeof(struct gaba_diff_vec_s) == 4 * BW);
#endif

/**
 * @struct gaba_char_vec_s
 */
struct gaba_char_vec_s {
	uint8_t w[BW];						/** (32) a in the lower 4bit, b in the higher 4bit */
};
_static_assert(sizeof(struct gaba_char_vec_s) == BW);

/**
 * @struct gaba_block_s
 * @brief a unit of banded matrix, 32 vector updates will be recorded in a single block object.
 * phantom is an alias of the block struct as a head cap of contiguous blocks.
 */
struct gaba_block_s {
	struct gaba_mask_pair_s mask[BLK];	/** (256 / 512) traceback capability flag vectors (set if transition to the ajdacent cell is possible) */
	struct gaba_diff_vec_s diff; 		/** (64, 128, 256) diff variables of the last vector */
	uint32_t dir_mask;					/** (4) extension direction bit array */
	int8_t acc, xstat;					/** (2) accumulator, and xdrop status (term detected when xstat < 0) */
	int8_t acnt, bcnt;					/** (2) forwarded lengths */
	uint64_t max_mask;					/** (8) lanewise update mask (set if the lane contains the current max) */
};
struct gaba_phantom_s {
	struct gaba_diff_vec_s diff; 		/** (64, 128, 256) diff variables of the last (just before the head) vector */
	uint32_t reserved;					/** (4) overlaps with dir_mask */
	int8_t acc, xstat;					/** (2) accumulator, and xdrop status (term detected when xstat < 0) */
	int8_t acnt, bcnt;					/** (4) prefetched sequence lengths (only effective at the root, otherwise zero) */
	struct gaba_block_s const *blk;		/** (8) link to the previous block (overlaps with max_mask) */
};
_static_assert(sizeof(struct gaba_block_s) % 16 == 0);
_static_assert(sizeof(struct gaba_phantom_s) % 16 == 0);
#define _last_block(x)				( (struct gaba_block_s *)(x) - 1 )
#define _last_phantom(x)			( (struct gaba_phantom_s *)(x) - 1 )

/**
 * @struct gaba_section_pair_s
 */
struct gaba_section_pair_s {
	uint8_t const *atail, *btail;		/** (16) tail of the current section */
	uint32_t alen, blen;				/** (8) lengths of the current section */
	uint32_t aid, bid;					/** (8) ids */
};

/**
 * @struct gaba_tail_pair_s
 * @brief used in merging vectors. keeps pointers to either of two merged tails, for each cell in two (the current and the previous) vectors.
 */
struct gaba_tail_pair_s {
	struct gaba_joint_tail_s const *tail[2];/** (16) merged two tails */
	uint64_t tail_idx_mask[2];			/** (16) 0/1 index array: [0] for the previous vector, [1] for the current vector (tail->md) */
};

/**
 * @struct gaba_joint_tail_s
 * @brief (internal) tail cap of a contiguous matrix blocks, contains a context of the blocks
 * (band) and can be connected to the next blocks.
 */
struct gaba_joint_tail_s {
	/* char vector and delta vectors */
	struct gaba_char_vec_s ch;			/** (16, 32, 64) char vector */
	struct gaba_drop_s xd;				/** (16, 32, 64) */
	struct gaba_middle_delta_s md;		/** (32, 64, 128) */

	int8_t qdiff[2], unused[2];			/** (4) displacement of two merged vectors */
	uint32_t pridx;						/** (4) remaining p-length */
	uint32_t aridx, bridx;				/** (8) reverse indices for the tails */
	uint32_t asridx, bsridx;			/** (8) start reverse indices (for internal use) */
	int64_t offset;						/** (8) large offset */
	struct gaba_fill_s f;				/** (24) */

	/* tail pointer */
	struct gaba_joint_tail_s const *tail;/** (8) the previous tail */

	/* section info or merged tails */
	union {
		struct gaba_section_pair_s s;	/** (32) */
		struct gaba_tail_pair_s t;		/** (32) */
	} u;
};
_static_assert((sizeof(struct gaba_joint_tail_s) % 32) == 0);
#define TAIL_BASE				( offsetof(struct gaba_joint_tail_s, f) )
#define _tail(x)				( (struct gaba_joint_tail_s *)((uint8_t *)(x) - TAIL_BASE) )
#define _fill(x)				( (struct gaba_fill_s *)((uint8_t *)(x) + TAIL_BASE) )

/**
 * @struct gaba_root_block_s
 */
struct gaba_root_block_s {
	uint8_t _pad1[288 - sizeof(struct gaba_phantom_s)];
	struct gaba_phantom_s blk;
	struct gaba_joint_tail_s tail;
#if BW != 64
	uint8_t _pad2[352 - sizeof(struct gaba_joint_tail_s)];
#endif
};
_static_assert(sizeof(struct gaba_root_block_s) == 640);
_static_assert(sizeof(struct gaba_root_block_s) >= sizeof(struct gaba_phantom_s) + sizeof(struct gaba_joint_tail_s));

/**
 * @struct gaba_reader_work_s
 * @brief (internal) working buffer for fill-in functions, contains sequence prefetch buffer
 * and middle/max-small deltas.
 */
struct gaba_reader_work_s {
	/** 64byte aligned */
	uint8_t bufa[BW_MAX + BLK];			/** (128) */
	uint8_t bufb[BW_MAX + BLK];			/** (128) */
	/** 256 */

	/** 64byte alidned */
	struct gaba_section_pair_s s;		/** (32) section pair */
	int32_t pridx, ofsd;				/** (4) delta of large offset */
	uint32_t aridx, bridx;				/** (8) current ridx */
	uint32_t asridx, bsridx;			/** (8) start ridx (converted to (badv, aadv) in fill_create_tail) */
	struct gaba_joint_tail_s const *tail;	/** (8) previous tail */
	/** 64 */

	/** 64byte aligned */
	struct gaba_drop_s xd;				/** (16, 32, 64) current drop from max */
#if BW != 64
	uint8_t _pad[BW == 16 ? 16 : 32];	/** padding to align to 64-byte boundary */
#endif
	struct gaba_middle_delta_s md;		/** (32, 64, 128) */
};
_static_assert((sizeof(struct gaba_reader_work_s) % 64) == 0);

/**
 * @struct gaba_aln_intl_s
 * @brief internal alias of gaba_alignment_t, allocated in the local context and used as a working buffer.
 */
struct gaba_aln_intl_s {
	/* memory management */
	void *opaque;						/** (8) opaque (context pointer) */
	gaba_lfree_t lfree;					/** (8) local free */

	uint32_t head_margin;				/** (8) margin size at the head of an alignment object, must be multiple of 8 */
	uint32_t slen;						/** (8) section length */
	struct gaba_segment_s *seg;			/** (8) */
	uint64_t plen;						/** (8) path length (psum) */
	int64_t score;						/** (8) score */
	uint32_t mcnt, xcnt;				/** (8) #matchs, #mismatchs */
	uint32_t gicnt, gecnt;				/** (8) #gap opens, #gap bases */
};
_static_assert(sizeof(struct gaba_alignment_s) == sizeof(struct gaba_aln_intl_s));
_static_assert(offsetof(struct gaba_alignment_s, slen) == offsetof(struct gaba_aln_intl_s, slen));
_static_assert(offsetof(struct gaba_alignment_s, seg) == offsetof(struct gaba_aln_intl_s, seg));
_static_assert(offsetof(struct gaba_alignment_s, plen) == offsetof(struct gaba_aln_intl_s, plen));
_static_assert(offsetof(struct gaba_alignment_s, mcnt) == offsetof(struct gaba_aln_intl_s, mcnt));
_static_assert(offsetof(struct gaba_alignment_s, xcnt) == offsetof(struct gaba_aln_intl_s, xcnt));
_static_assert(offsetof(struct gaba_alignment_s, gicnt) == offsetof(struct gaba_aln_intl_s, gicnt));
_static_assert(offsetof(struct gaba_alignment_s, gecnt) == offsetof(struct gaba_aln_intl_s, gecnt));

/**
 * @struct gaba_leaf_s
 * @brief working buffer for max score search
 */
struct gaba_leaf_s {
	struct gaba_joint_tail_s const *tail;
	struct gaba_block_s const *blk;
	uint32_t p, q;						/** (8) local p (to restore mask pointer), local q */
	uint64_t ppos;						/** (8) global p (== resulting path length) */
	uint32_t aridx, bridx;
};

/**
 * @struct gaba_writer_work_s
 * @brief working buffer for traceback (allocated in the thread-local context)
 */
struct gaba_writer_work_s {
	/** local context */
	struct gaba_aln_intl_s a;			/** (64) working buffer, copied to the result object */

	/** work */
	uint32_t state;						/** (4) d/v0/v1/h0/h1 */
	uint32_t ofs;						/** (4) path array offset */
	uint32_t *path;						/** (8) path array pointer */
	struct gaba_block_s const *blk;		/** (8) current block */
	uint32_t p, q;						/** (8) local p, q-coordinate, [0, BW) */

	/** save */
	uint32_t agidx, bgidx;				/** (8) grid indices of the current trace */
	uint32_t asgidx, bsgidx;			/** (8) base indices of the current trace */
	uint32_t aid, bid;					/** (8) section ids */

	/** section info */
	struct gaba_joint_tail_s const *atail;/** (8) */
	struct gaba_joint_tail_s const *btail;/** (8) */
	struct gaba_alignment_s *aln;		/** (8) */

	struct gaba_leaf_s leaf;			/** (40) working buffer for max pos search */
	uint64_t _pad1;
	/** 64, 192 */
};
_static_assert((sizeof(struct gaba_writer_work_s) % 64) == 0);

/**
 * @struct gaba_score_vec_s
 */
struct gaba_score_vec_s {
	int8_t v1[16];
	int8_t v2[16];
	int8_t v3[16];
	int8_t v4[16];
	int8_t v5[16];
};
_static_assert(sizeof(struct gaba_score_vec_s) == 80);

/**
 * @struct gaba_mem_block_s
 */
struct gaba_mem_block_s {
	struct gaba_mem_block_s *next;
	struct gaba_mem_block_s *prev;
	uint64_t size;
};
_static_assert(sizeof(struct gaba_mem_block_s) == 24);

/**
 * @struct gaba_stack_s
 * @brief save stack pointer
 */
struct gaba_stack_s {
	struct gaba_mem_block_s *mem;
	uint8_t *top;
	uint8_t *end;
};
_static_assert(sizeof(struct gaba_stack_s) == 24);

/**
 * @struct gaba_dp_context_s
 *
 * @brief (internal) container for dp implementations
 */
struct gaba_dp_context_s {
	/* working buffers */
	union gaba_work_s {
		struct gaba_reader_work_s r;	/** (192) */
		struct gaba_writer_work_s l;	/** (192) */
	} w;
	/** 64byte aligned */

	/** loaded on init */
	struct gaba_score_vec_s scv;		/** (80) substitution matrix and gaps */

	/* tail-of-userland pointers */
	uint8_t const *alim, *blim;			/** (16) max index of seq array */

	/* scores */
	int8_t tx;							/** (1) xdrop threshold */
	int8_t tf;							/** (1) filter threshold */
	uint8_t _pad1[6];

	/** output options */
	uint32_t head_margin;				/** (1) margin at the head of gaba_res_t */
	uint32_t tail_margin;				/** (1) margin at the tail of gaba_res_t */

	/* memory management */
	struct gaba_mem_block_s mem;		/** (24) root memory block */
	struct gaba_stack_s stack;			/** (24) current stack */
	/** 64byte aligned */
	
	/** phantom vectors */
	struct gaba_joint_tail_s const *root[4];	/** (32) root tail */
	/** 128; 64byte aligned */
};
_static_assert((sizeof(struct gaba_dp_context_s) % 64) == 0);
#define GABA_DP_CONTEXT_LOAD_OFFSET	( offsetof(struct gaba_dp_context_s, scv) )
#define GABA_DP_CONTEXT_LOAD_SIZE	( sizeof(struct gaba_dp_context_s) - GABA_DP_CONTEXT_LOAD_OFFSET )
_static_assert((GABA_DP_CONTEXT_LOAD_OFFSET % 64) == 0);
_static_assert((GABA_DP_CONTEXT_LOAD_SIZE % 64) == 0);
#define _root(_t)					( (_t)->root[_dp_ctx_index(BW)] )

/**
 * @struct gaba_opaque_s
 */
struct gaba_opaque_s {
	void *api[4];
};
#define _export_dp_context(_t) ( \
	(struct gaba_dp_context_s *)(((struct gaba_opaque_s *)(_t)) - DP_CTX_MAX + _dp_ctx_index(BW)) \
)
#define _restore_dp_context(_t) ( \
	(struct gaba_dp_context_s *)(((struct gaba_opaque_s *)(_t)) - _dp_ctx_index(BW) + DP_CTX_MAX) \
)
#define _export_dp_context_global(_t) ( \
	(struct gaba_dp_context_s *)(((struct gaba_opaque_s *)(_t)) - DP_CTX_MAX + _dp_ctx_index(BW)) \
)
#define _restore_dp_context_global(_t) ( \
	(struct gaba_dp_context_s *)(((struct gaba_opaque_s *)(_t)) - _dp_ctx_index(BW) + DP_CTX_MAX) \
)

/**
 * @struct gaba_context_s
 *
 * @brief (API) an algorithmic context.
 *
 * @sa gaba_init, gaba_close
 */
struct gaba_context_s {
	/** opaque pointers for function dispatch */
	struct gaba_opaque_s api[4];		/** function dispatcher, used in gaba_wrap.h */
	/** 64byte aligned */

	/** templates */
	struct gaba_dp_context_s dp;		/** template of thread-local context */
	/** 64byte aligned */

	/** phantom vectors */
	struct gaba_root_block_s ph[3];		/** (768) template of root vectors, [0] for 16-cell, [1] for 32-cell, [2] for 64-cell */
	/** 64byte aligned */
};
_static_assert((sizeof(struct gaba_dp_context_s) % 64) == 0);
#define _proot(_c, _bw)				( &(_c)->ph[_dp_ctx_index(_bw)] )

/**
 * @enum GABA_BLK_STATUS
 */
enum GABA_BLK_STATUS {
	CONT			= 0,
	UPDATE			= 0x01,
	TERM			= 0x02,
	STAT_MASK		= UPDATE | TERM | CONT,
	HEAD			= 0x10,
	MERGE_HEAD		= 0x20,				/* merged head and the corresponding block contains no actual vector (DP cell) */
	ROOT			= 0x40				/* update flag will be combined set for the actual root */
};
_static_assert((int32_t)CONT<<8 == (int32_t)GABA_CONT);
_static_assert((int32_t)UPDATE<<8 == (int32_t)GABA_UPDATE);
_static_assert((int32_t)TERM<<8 == (int32_t)GABA_TERM);


/**
 * coordinate conversion macros
 */
#define _rev(pos, len)				( (len) + (uint64_t)(len) - (uint64_t)(pos) - 1 )
#define _roundup(x, base)			( ((x) + (base) - 1) & ~((base) - 1) )

/**
 * max and min
 */
#define MAX2(x,y) 		( (x) > (y) ? (x) : (y) )
#define MAX3(x,y,z) 	( MAX2(x, MAX2(y, z)) )
#define MAX4(w,x,y,z) 	( MAX2(MAX2(w, x), MAX2(y, z)) )

#define MIN2(x,y) 		( (x) < (y) ? (x) : (y) )
#define MIN3(x,y,z) 	( MIN2(x, MIN2(y, z)) )
#define MIN4(w,x,y,z) 	( MIN2(MIN2(w, x), MIN2(y, z)) )


/**
 * @fn gaba_malloc, gaba_free
 * @brief a pair of malloc and free, aligned and margined.
 * any pointer created by gaba_malloc MUST be freed by gaba_free.
 */
static _force_inline
void *gaba_malloc(
	size_t size)
{
	void *ptr = NULL;

	/* roundup to align boundary, add margin at the head and tail */
	size = _roundup(size, MEM_ALIGN_SIZE);
	if(posix_memalign(&ptr, MEM_ALIGN_SIZE, size + 2 * MEM_MARGIN_SIZE) != 0) {
		debug("posix_memalign failed");
		return(NULL);
	}
	debug("posix_memalign(%p)", ptr);
	return(ptr + MEM_MARGIN_SIZE);
}
static _force_inline
void gaba_free(
	void *ptr)
{
	free(ptr - MEM_MARGIN_SIZE);
	return;
}


/* matrix fill functions */

/* direction macros */
/**
 * @struct gaba_dir_s
 */
struct gaba_dir_s {
	uint32_t mask;
	int8_t acc;
};

/**
 * @macro _dir_init
 */
#define _dir_init(_blk) ((struct gaba_dir_s){ .mask = 0, .acc = (_blk)->acc })
/**
 * @macro _dir_fetch
 */
#define _dir_fetch(_d) { \
	(_d).mask <<= 1; (_d).mask |= (uint32_t)((_d).acc < 0);  \
	debug("fetched dir(%x), %s", (_d).mask, _dir_is_down(_d) ? "go down" : "go right"); \
}
/**
 * @macro _dir_update
 * @brief update direction determiner for the next band
 */
#define _dir_update(_d, _vector, _sign) { \
	(_d).acc += (_sign) * (_ext_n(_vector, 0) - _ext_n(_vector, BW-1)); \
	/*debug("acc(%d), (%d, %d)", _dir_acc, _ext_n(_vector, 0), _ext_n(_vector, BW-1));*/ \
}
/**
 * @macro _dir_adjust_remainder
 * @brief adjust direction array when termination is detected in the middle of the block
 */
#define _dir_adjust_remainder(_d, _filled_count) { \
	debug("adjust remainder, array(%x), shifted array(%x)", (_d).mask, (_d).mask<<(BLK - (_filled_count))); \
	(_d).mask <<= (BLK - (_filled_count)); \
}
/**
 * @macro _dir_is_down, _dir_is_right
 * @brief direction indicator (_dir_is_down returns true if dir == down)
 */
#define _dir_mask_is_down(_mask)	( (_mask) & 0x01 )
#define _dir_mask_is_right(_mask)	( ~(_mask) & 0x01 )
#define _dir_is_down(_d)			( _dir_mask_is_down((_d).mask) )
#define _dir_is_right(_d)			( _dir_mask_is_right((_d).mask) )
#define _dir_bcnt(_d)				( popcnt((_d).mask) )				/* count vertical transitions */
#define _dir_mask_windback(_mask)	{ (_mask) >>= 1; }
#define _dir_windback(_d)			{ (_d).mask >>= 1; }
/**
 * @macro _dir_save
 */
#define _dir_save(_blk, _d) { \
	(_blk)->dir_mask = (_d).mask;	/* store mask */ \
	(_blk)->acc = (_d).acc;			/* store accumulator */ \
}
/**
 * @macro _dir_load
 */
#if 1
#define _dir_mask_load(_blk, _cnt)	( (_blk)->dir_mask>>(BLK - (_cnt)) )
#define _dir_load(_blk, _cnt) ( \
	(struct gaba_dir_s){ \
		.mask = _dir_mask_load(_blk, _cnt), \
		.acc = (_blk)->acc \
	} \
	/*debug("load dir cnt(%d), mask(%x), shifted mask(%x)", (int32_t)_filled_count, _d.mask, _d.mask>>(BLK - (_filled_count)));*/ \
)
#else
#define _dir_load(_blk, _local_idx) ({ \
	struct gaba_dir_s _d = (struct gaba_dir_s){ \
		.mask = (_blk)->dir_mask, \
		.acc = 0 \
	}; \
	debug("load dir idx(%d), mask(%x), shifted mask(%x)", (int32_t)_local_idx, _d.mask, _d.mask>>(BLK - (_local_idx) - 1)); \
	_d.mask >>= (BLK - (_local_idx) - 1); \
	_d; \
})
#endif

/**
 * seqreader macros
 */
#define _rd_bufa_base(k)		( (k)->w.r.bufa + BLK + BW )
#define _rd_bufb_base(k)		( (k)->w.r.bufb )
#define _rd_bufa(k, pos, len)	( _rd_bufa_base(k) - (pos) - (len) )
#define _rd_bufb(k, pos, len)	( _rd_bufb_base(k) + (pos) )
#define _lo64(v)				_ext_v2i64(v, 0)
#define _hi64(v)				_ext_v2i64(v, 1)
#define _lo32(v)				_ext_v2i32(v, 0)
#define _hi32(v)				_ext_v2i32(v, 1)

/* sequence encoding */

/**
 * @enum BASES
 */
#if BIT == 2
enum BASES { A = 0x00, C = 0x01, G = 0x02, T = 0x03, N = 0x80 };
#else
enum BASES { A = 0x01, C = 0x02, G = 0x04, T = 0x08, N = 0x00 };
#endif

/**
 * @macro _adjust_BLK, _comp_BLK, _match_BW
 */
#if BIT == 2
#  define _adjust_v16i8(_v)		_shl_v16i8(_v, 2)
#  define _adjust_v32i8(_v)		_shl_v32i8(_v, 2)
#  define _comp_v16i8(_c, _v)	_xor_v16i8(_c, _v)
#  define _comp_v32i8(_c, _v)	_xor_v32i8(_c, _v)
#  define _match_n(_a, _b)		_or_n(_a, _b)

static uint8_t const comp_mask[16] __attribute__(( aligned(16) )) = {
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03,
	0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03
};

#else
#  define _adjust_v16i8(_v)		(_v)
#  define _adjust_v32i8(_v)		(_v)
#  define _comp_v16i8(_c, _v)	_shuf_v16i8(_c, _v)
#  define _comp_v32i8(_c, _v)	_shuf_v32i8(_c, _v)
#  define _match_n(_a, _b)		_and_n(_a, _b)

static uint8_t const comp_mask[16] __attribute__(( aligned(16) )) = {
	0x00, 0x08, 0x04, 0x0c, 0x02, 0x0a, 0x06, 0x0e,
	0x01, 0x09, 0x05, 0x0d, 0x03, 0x0b, 0x07, 0x0f
};

#endif


/**
 * @fn fill_fetch_seq_a
 */
static _force_inline
void fill_fetch_seq_a(
	struct gaba_dp_context_s *self,
	uint8_t const *pos,
	uint64_t len)
{
	if(pos < self->alim) {
		debug("reverse fetch a: pos(%p), len(%llu)", pos, len);
		/* reverse fetch: 2 * alen - (2 * alen - pos) + (len - 32) */
		v32i8_t ach = _loadu_v32i8(pos + (len - BLK));
		_storeu_v32i8(_rd_bufa(self, BW, len), _swap_v32i8(ach));		/* reverse */
	} else {
		debug("forward fetch a: pos(%p), len(%llu)", pos, len);
		/* forward fetch: 2 * alen - pos */
		v32i8_t const cv = _from_v16i8_v32i8(_load_v16i8(comp_mask));	/* complement mask */
		v32i8_t ach = _loadu_v32i8(_rev(pos + (len - 1), self->alim));
		_storeu_v32i8(_rd_bufa(self, BW, len), _comp_v32i8(cv, ach));	/* complement */
	}
	return;
}

/**
 * @fn fill_fetch_seq_a_n
 * FIXME: this function invades 16bytes before bufa.
 */
static _force_inline
void fill_fetch_seq_a_n(
	struct gaba_dp_context_s *self,
	uint64_t ofs,
	uint8_t const *pos,
	uint64_t len)
{
	if(pos < self->alim) {
		debug("reverse fetch a: pos(%p), len(%llu)", pos, len);
		/* reverse fetch: 2 * alen - (2 * alen - pos) + (len - 32) */
		pos += len; ofs += len;		/* fetch in reverse direction */
		while(len > 0) {
			uint64_t l = MIN2(len, 16);
			v16i8_t ach = _loadu_v16i8(pos - 16);
			_storeu_v16i8(_rd_bufa(self, ofs - l, l), _swap_v16i8(ach));	/* reverse */
			len -= l; pos -= l; ofs -= l;
		}
	} else {
		debug("forward fetch a: pos(%p), len(%llu)", pos, len);
		/* forward fetch: 2 * alen - pos */
		v16i8_t const cv = _load_v16i8(comp_mask);		/* complement mask */
		pos += len - 1; ofs += len;
		while(len > 0) {
			uint64_t l = MIN2(len, 16);
			v16i8_t ach = _loadu_v16i8(_rev(pos, self->alim));
			_storeu_v16i8(_rd_bufa(self, ofs - l, l), _comp_v16i8(cv, ach));	/* complement */
			len -= l; pos -= l; ofs -= l;
		}
	}
	return;
}

/**
 * @fn fill_fetch_seq_b
 */
static _force_inline
void fill_fetch_seq_b(
	struct gaba_dp_context_s *self,
	uint8_t const *pos,
	uint64_t len)
{
	if(pos < self->blim) {
		debug("forward fetch b: pos(%p), len(%llu)", pos, len);
		/* forward fetch: pos */
		v32i8_t bch = _loadu_v32i8(pos);
		_storeu_v32i8(_rd_bufb(self, BW, len), _adjust_v32i8(bch));		/* forward */
	} else {
		debug("reverse fetch b: pos(%p), len(%llu)", pos, len);
		/* reverse fetch: 2 * blen - pos + (len - 32) */
		v32i8_t const cv = _from_v16i8_v32i8(_load_v16i8(comp_mask));	/* complement mask */
		v32i8_t bch = _loadu_v32i8(_rev(pos, self->blim) - (BLK - 1));
		_storeu_v32i8(_rd_bufb(self, BW, len),
			_adjust_v32i8(_comp_v32i8(cv, _swap_v32i8(bch)))			/* reverse complement */
		);
	}
	return;
}

/**
 * @fn fill_fetch_seq_b_n
 * FIXME: this function invades 16bytes after bufb.
 */
static _force_inline
void fill_fetch_seq_b_n(
	struct gaba_dp_context_s *self,
	uint64_t ofs,
	uint8_t const *pos,
	uint64_t len)
{
	if(pos < self->blim) {
		debug("forward fetch b: pos(%p), len(%llu)", pos, len);
		/* forward fetch: pos */
		while(len > 0) {
			uint64_t l = MIN2(len, 16);					/* advance length */
			v16i8_t bch = _loadu_v16i8(pos);
			_storeu_v16i8(_rd_bufb(self, ofs, l), _adjust_v16i8(bch));
			len -= l; pos += l; ofs += l;
		}
	} else {
		debug("reverse fetch b: pos(%p), len(%llu)", pos, len);
		/* reverse fetch: 2 * blen - pos + (len - 16) */
		v16i8_t const cv = _load_v16i8(comp_mask);		/* complement mask */
		while(len > 0) {
			uint64_t l = MIN2(len, 16);					/* advance length */
			v16i8_t bch = _loadu_v16i8(_rev(pos + (16 - 1), self->blim));
			_storeu_v16i8(_rd_bufb(self, ofs, l),
				_adjust_v16i8(_comp_v16i8(cv, _swap_v16i8(bch)))
			);
			len -= l; pos += l; ofs += l;
		}
	}
	return;
}

/**
 * @fn fill_fetch_core
 * @brief fetch sequences from current sections, lengths must be shorter than 32 (= BLK)
 */
static _force_inline
void fill_fetch_core(
	struct gaba_dp_context_s *self,
	uint32_t acnt,
	uint32_t alen,
	uint32_t bcnt,
	uint32_t blen)
{
	/* fetch seq a */
	nvec_t a = _loadu_n(_rd_bufa(self, acnt, BW));		/* unaligned */
	fill_fetch_seq_a(self, self->w.r.s.atail - self->w.r.aridx, alen);
	_store_n(_rd_bufa(self, 0, BW), a);					/* always aligned */

	/* fetch seq b */
	nvec_t b = _loadu_n(_rd_bufb(self, bcnt, BW));		/* unaligned */
	_store_n(_rd_bufb(self, 0, BW), b);					/* always aligned */
	fill_fetch_seq_b(self, self->w.r.s.btail - self->w.r.bridx, blen);
	return;
}

/**
 * @fn fill_cap_fetch
 */
static _force_inline
void fill_cap_fetch(
	struct gaba_dp_context_s *self,
	struct gaba_block_s const *blk)
{
	/* fetch: len might be clipped by ridx */
	v2i32_t const ridx = _load_v2i32(&self->w.r.aridx);
	v2i32_t const lim = _set_v2i32(BLK);
	v2i32_t len = _min_v2i32(ridx, lim);
	_print_v2i32(len);
	fill_fetch_core(self, (blk - 1)->acnt, _lo32(len), (blk - 1)->bcnt, _hi32(len));
	return;
}

/**
 * @fn fill_init_fetch
 * @brief similar to cap fetch, updating ridx and rem
 */
static _force_inline
int64_t fill_init_fetch(
	struct gaba_dp_context_s *self,
	struct gaba_block_s *blk,
	int64_t ppos)								/* ppos can be loaded from self->w.r.tail->ppos */
{
	/* restore remaining head margin lengths */
	#if 0
	v2i32_t const adj = _seta_v2i32(0, 1);
	v2i32_t rem = _sar_v2i32(_add_v2i32(_set_v2i32(-ppos), adj), 1);
	#else
	v2i32_t const adj = _seta_v2i32(1, 0);
	// v2i32_t const adj = _seta_v2i32(0, 1);
	v2i32_t rem = _sar_v2i32(_sub_v2i32(_set_v2i32(-ppos), adj), 1);
	#endif

	/* load ridx: no need to update ridx since advance counters are always zero */
	v2i32_t ridx = _load_v2i32(&self->w.r.aridx);

	/* fetch, bounded by remaining sequence lengths and remaining head margins */
	v2i32_t len = _min_v2i32(
		_min_v2i32(
			rem,								/* if remaining head margin is the shortest */
			ridx								/* if remaining sequence length is the shortest */
		),
		_add_v2i32(								/* if the opposite sequence length is the shortest */
			_swap_v2i32(_sub_v2i32(ridx, rem)),
			_add_v2i32(adj, rem)
		)
	);
	debug("ppos(%lld)", ppos);
	_print_v2i32(rem);
	_print_v2i32(ridx);
	_print_v2i32(len);

	/* fetch sequence and store at (0, 0), then save newly loaded sequence lengths */
	fill_fetch_core(self, 0, _lo32(len), 0, _hi32(len));
	_print_n(_load_n(_rd_bufa(self, 0, BW)));
	_print_n(_load_n(_rd_bufb(self, 0, BW)));

	/* save fetch length for use in the next block fill / tail construction */
	_store_v2i8(&blk->acnt, _cvt_v2i32_v2i8(len));
	_store_v2i32(&self->w.r.aridx, _sub_v2i32(ridx, len));
	return(ppos + _lo32(len) + _hi32(len));
}

/**
 * @fn fill_restore_fetch
 * @brief fetch sequence from an existing block
 */
static _force_inline
void fill_restore_fetch(
	struct gaba_dp_context_s *self,
	struct gaba_joint_tail_s const *tail,
	struct gaba_block_s const *blk,
	v2i32_t ridx)
{
	/* load segment head info */
	struct gaba_joint_tail_s const *prev_tail = tail->tail;
	v2i32_t sridx = _load_v2i32(&tail->asridx);
	_print_v2i32(ridx); _print_v2i32(sridx);

	/* calc fetch positions and lengths */
	v2i32_t dridx = _add_v2i32(ridx, _set_v2i32(BW));	/* desired fetch pos */
	v2i32_t cridx = _min_v2i32(dridx, sridx);			/* might be clipped at the head of sequence section */
	v2i32_t ofs = _sub_v2i32(dridx, cridx);				/* lengths fetched from phantom */
	v2i32_t len = _min_v2i32(
		cridx,											/* clipped at the tail of sequence section */
		_sub_v2i32(_set_v2i32(BW_MAX + BLK), ofs)		/* lengths to fill buffers */
	);
	_print_v2i32(dridx); _print_v2i32(cridx);
	_print_v2i32(ofs); _print_v2i32(len);

	/* init buffer with magic (for debugging) */
	_memset_blk_a(self->w.r.bufa, 0, 2 * (BW_MAX + BLK));

	/* fetch seq a */
	fill_fetch_seq_a_n(self, _lo32(ofs), tail->u.s.atail - _lo32(cridx), _lo32(len));
	if(_lo32(ofs) > 0) {
		nvec_t ach = _and_n(_set_n(0x0f), _loadu_n(&prev_tail->ch));/* aligned to 16byte boundaries */
		_print_n(ach);
		_storeu_n(_rd_bufa(self, 0, _lo32(ofs)), ach);				/* invades backward */
	}

	/* fetch seq b */
	if(_hi32(ofs) > 0) {
		nvec_t bch = _and_n(
			_set_n(0x0f),
			_shr_n(_loadu_n(&prev_tail->ch.w[BW - _hi32(ofs)]), 4)	/* invades backward */
		);
		_print_n(bch);
		_storeu_n(_rd_bufb(self, 0, _hi32(ofs)), bch);				/* aligned store */
	}
	fill_fetch_seq_b_n(self, _hi32(ofs), tail->u.s.btail - _hi32(cridx), _hi32(len));
	return;
}

/*
 * @fn fill_load_section
 */
static _force_inline
void fill_load_section(
	struct gaba_dp_context_s *self,
	struct gaba_section_s const *a,
	struct gaba_section_s const *b,
	uint64_t _ridx,								/* (bridx, aridx) */
	uint32_t pridx)
{
	/* load current section lengths */
	v2i64_t asec = _loadu_v2i64(a);				/* tuple of (64bit ptr, 32-bit id, 32-bit len) */
	v2i64_t bsec = _loadu_v2i64(b);

	/* transpose sections */
	v2i32_t aid_alen = _cast_v2i64_v2i32(asec);	/* extract lower 64bit */
	v2i32_t bid_blen = _cast_v2i64_v2i32(bsec);

	v2i32_t id = _lo_v2i32(aid_alen, bid_blen);
	v2i32_t len = _hi_v2i32(aid_alen, bid_blen);
	v2i64_t base = _hi_v2i64(asec, bsec);

	/* calc tail pointer */
	v2i64_t tail = _add_v2i64(base, _cvt_v2i32_v2i64(len));
	_print_v2i32(id);
	_print_v2i32(len);
	_print_v2i64(tail);

	/* store sections */
	_store_v2i64(&self->w.r.s.atail, tail);
	_store_v2i32(&self->w.r.s.alen, len);
	_store_v2i32(&self->w.r.s.aid, id);

	/* calc ridx */
	v2i32_t ridx = _cvt_u64_v2i32(_ridx);
	ridx = _sel_v2i32(_eq_v2i32(ridx, _zero_v2i32()),/* if ridx is zero (occurs when section is updated) */
		len,									/* load the next section */
		ridx									/* otherwise use the same section as the previous */
	);
	self->w.r.pridx = pridx;					/* remaining p length, utilized in merging vectors */
	_store_v2i32(&self->w.r.aridx, ridx);
	_store_v2i32(&self->w.r.asridx, ridx);
	_print_v2i32(ridx);
	return;
}

/**
 * @fn fill_create_phantom
 * @brief initialize phantom block
 */
static _force_inline
struct gaba_block_s *fill_create_phantom(
	struct gaba_dp_context_s *self,
	struct gaba_block_s const *prev_blk)
{
	struct gaba_phantom_s *ph = (struct gaba_phantom_s *)self->stack.top;
	debug("start stack_top(%p), stack_end(%p)", self->stack.top, self->stack.end);

	/* head sequence buffers are already filled, continue to body fill-in (first copy phantom block) */
	_memcpy_blk_uu(&ph->diff, &prev_blk->diff, sizeof(struct gaba_diff_vec_s));
	ph->reserved = 0;							/* overlaps with mask */
	ph->acc = prev_blk->acc;					/* just copy */
	ph->xstat = (prev_blk->xstat & (ROOT | UPDATE)) | HEAD;	/* propagate root-update flag and mark head */
	ph->acnt = ph->bcnt = 0;					/* clear counter (a and b sequences are aligned at the head of the buffer) FIXME: intermediate seq fetch breaks consistency */
	ph->blk = prev_blk;
	debug("ph(%p), xstat(%x)", ph, ph->xstat);
	return((struct gaba_block_s *)(ph + 1) - 1);
}

/**
 * @fn fill_load_tail
 * @brief load sequences and indices from the previous tail
 */
static _force_inline
struct gaba_block_s *fill_load_tail(
	struct gaba_dp_context_s *self,
	struct gaba_joint_tail_s const *tail,
	struct gaba_section_s const *a,
	struct gaba_section_s const *b,
	uint64_t ridx,								/* (bridx, aridx) */
	uint32_t pridx)
{
	/* load sequences and sections */
	fill_load_section(self, a, b, ridx, pridx);
	self->w.r.tail = tail;

	/* clear offset */
	self->w.r.ofsd = 0;

	/* load sequence vectors */
	nvec_t const mask = _set_n(0x0f);
	nvec_t ch = _loadu_n(&tail->ch.w);
	nvec_t ach = _and_n(mask, ch);
	nvec_t bch = _and_n(mask, _shr_n(ch, 4));	/* bit 7 must be cleared not to affect shuffle mask */
	_store_n(_rd_bufa(self, 0, BW), ach);
	_store_n(_rd_bufb(self, 0, BW), bch);
	_print_n(ach); _print_n(bch);

	/* copy max and middle delta vectors */
	nvec_t xd = _loadu_n(&tail->xd);
	wvec_t md = _loadu_w(&tail->md);
	_store_n(&self->w.r.xd, xd);
	_store_w(&self->w.r.md, md);
	_print_n(xd);
	_print_w(md);

	/* extract the last block pointer, pass to fill-in loop */
	return(fill_create_phantom(self, _last_block(tail)));
}

/**
 * @fn fill_create_tail
 * @brief create joint_tail at the end of the blocks
 */
static _force_inline
struct gaba_joint_tail_s *fill_create_tail(
	struct gaba_dp_context_s *self,
	struct gaba_block_s *blk)
{
	/* create joint_tail */
	struct gaba_joint_tail_s *tail = (struct gaba_joint_tail_s *)(blk + 1);
	self->stack.top = (void *)(tail + 1);	/* write back stack_top */
	debug("end stack_top(%p), stack_end(%p), blk(%p)", self->stack.top, self->stack.end, blk);

	/* store char vector */
	nvec_t ach = _loadu_n(_rd_bufa(self, blk->acnt, BW));
	nvec_t bch = _loadu_n(_rd_bufb(self, blk->bcnt, BW));
	_print_n(ach); _print_n(bch);
	_storeu_n(&tail->ch, _or_n(ach, _shl_n(bch, 4)));

	/* load previous tail pointer */
	struct gaba_joint_tail_s const *prev_tail = self->w.r.tail;

	/* copy delta vectors */
	nvec_t xd = _load_n(&self->w.r.xd);
	wvec_t md = _load_w(&self->w.r.md);
	_storeu_n(&tail->xd, xd);
	_storeu_w(&tail->md, md);
	_print_n(xd);
	_print_w(md);

	/* search max section */
	md = _add_w(md, _cvt_n_w(xd));					/* xd holds drop from max */
	int64_t offset = prev_tail->offset + self->w.r.ofsd;
	int64_t max = ((int16_t)_hmax_w(md)) + offset;
	debug("prev_offset(%lld), offset(%lld), max(%d, %lld)", prev_tail->offset, offset, _hmax_w(md), max);

	/* reverse indices */
	tail->pridx = self->w.r.pridx;
	v2i32_t ridx = _load_v2i32(&self->w.r.aridx);
	v2i32_t sridx = _load_v2i32(&self->w.r.asridx);
	_store_v2i32(&tail->aridx, ridx);
	_store_v2i32(&tail->asridx, sridx);
	_print_v2i32(ridx);
	_print_v2i32(sridx);

	/* store scores */
	tail->offset = offset;
	tail->f.max = max;

	/* status flags, coordinates, link pointer, and sections */
	v2i32_t update = _eq_v2i32(ridx, _zero_v2i32());
	v2i32_t adv = _sub_v2i32(sridx, ridx);
	tail->f.stat = ((uint32_t)(blk->xstat & (UPDATE | TERM | CONT))<<8) | _mask_v2i32(update);
	tail->f.scnt = prev_tail->f.scnt - _hi32(update) - _lo32(update);
	tail->f.ppos = prev_tail->f.ppos + _hi32(adv) + _lo32(adv);
	tail->tail = prev_tail;
	_memcpy_blk_ua(&tail->u.s.atail, &self->w.r.s.atail, sizeof(struct gaba_section_pair_s));
	return(tail);
}


#undef _LOG_H_INCLUDED
#undef DEBUG
#include "log.h"

/**
 * @macro _fill_load_context
 * @brief load vectors onto registers
 */
#if MODEL == LINEAR
#define _fill_load_context(_blk) \
	debug("blk(%p)", (_blk)); \
	/* load sequence buffer offset */ \
	uint8_t const *aptr = _rd_bufa(self, 0, BW); \
	uint8_t const *bptr = _rd_bufb(self, 0, BW); \
	/* load mask pointer */ \
	struct gaba_mask_pair_s *ptr = ((struct gaba_block_s *)(_blk))->mask; \
	/* load vector registers */ \
	register nvec_t dh = _loadu_n(((_blk) - 1)->diff.dh); \
	register nvec_t dv = _loadu_n(((_blk) - 1)->diff.dv); \
	_print_n(_add_n(dh, _load_ofsh(self->scv))); \
	_print_n(_add_n(dv, _load_ofsv(self->scv))); \
	/* load delta vectors */ \
	register nvec_t delta = _zero_n(); \
	register nvec_t drop = _load_n(self->w.r.xd.drop); \
	_print_n(drop); \
	_print_w(_add_w(_load_w(&self->w.r.md), _add_w(_cvt_n_w(delta), _set_w(self->w.r.tail->offset + self->w.r.ofsd - 128)))); \
	_print_w(_add_w(_add_w(_load_w(&self->w.r.md), _cvt_n_w(delta)), _add_w(_cvt_n_w(drop), _set_w(self->w.r.tail->offset + self->w.r.ofsd)))); \
	/* load direction determiner */ \
	struct gaba_dir_s dir = _dir_init((_blk) - 1);
#else	/* AFFINE and COMBINED */
#define _fill_load_context(_blk) \
	debug("blk(%p)", (_blk)); \
	/* load sequence buffer offset */ \
	uint8_t const *aptr = _rd_bufa(self, 0, BW); \
	uint8_t const *bptr = _rd_bufb(self, 0, BW); \
	/* load mask pointer */ \
	struct gaba_mask_pair_s *ptr = ((struct gaba_block_s *)(_blk))->mask; \
	/* load vector registers */ \
	register nvec_t dh = _loadu_n(((_blk) - 1)->diff.dh); \
	register nvec_t dv = _loadu_n(((_blk) - 1)->diff.dv); \
	register nvec_t de = _loadu_n(((_blk) - 1)->diff.de); \
	register nvec_t df = _loadu_n(((_blk) - 1)->diff.df); \
	_print_n(_add_n(dh, _load_ofsh(self->scv))); \
	_print_n(_add_n(dv, _load_ofsv(self->scv))); \
	_print_n(_sub_n(_sub_n(de, dv), _load_adjh(self->scv))); \
	_print_n(_sub_n(_add_n(df, dh), _load_adjv(self->scv))); \
	/* load delta vectors */ \
	register nvec_t delta = _zero_n(); \
	register nvec_t drop = _load_n(self->w.r.xd.drop); \
	_print_n(drop); \
	_print_w(_add_w(_load_w(&self->w.r.md), _add_w(_cvt_n_w(delta), _set_w(self->w.r.tail->offset + self->w.r.ofsd - 128)))); \
	_print_w(_add_w(_add_w(_load_w(&self->w.r.md), _cvt_n_w(delta)), _add_w(_cvt_n_w(drop), _set_w(self->w.r.tail->offset + self->w.r.ofsd)))); \
	/* load direction determiner */ \
	struct gaba_dir_s dir = _dir_init((_blk) - 1);
#endif

/**
 * @macro _fill_body
 * @brief update vectors
 */
#if MODEL == LINEAR
#define _fill_body() { \
	register nvec_t t = _match_n(_loadu_n(aptr), _loadu_n(bptr)); \
	_print_n(_loadu_n(aptr)); _print_n(_loadu_n(bptr)); \
	t = _shuf_n(_load_sb(self->scv), t); _print_n(t); \
	t = _max_n(dh, t); \
	t = _max_n(dv, t); \
	ptr->h.mask = _mask_n(_eq_n(t, dv)); \
	ptr->v.mask = _mask_n(_eq_n(t, dh)); \
	debug("mask(%x, %x)", ptr->h.all, ptr->v.all); \
	ptr++; \
	nvec_t _dv = _sub_n(t, dh); \
	dh = _sub_n(t, dv); \
	dv = _dv; \
	_print_n(drop); \
	_print_n(_add_n(dh, _load_ofsh(self->scv))); \
	_print_n(_add_n(dv, _load_ofsv(self->scv))); \
}
#elif MODEL == AFFINE
#define _fill_body() { \
	register nvec_t t = _match_n(_loadu_n(aptr), _loadu_n(bptr)); \
	_print_n(_loadu_n(aptr)); _print_n(_loadu_n(bptr)); \
	t = _shuf_n(_load_sb(self->scv), t); _print_n(t); \
	t = _max_n(de, t); \
	t = _max_n(df, t); \
	ptr->h.mask = _mask_n(_eq_n(t, de)); \
	ptr->v.mask = _mask_n(_eq_n(t, df)); \
	debug("mask(%x, %x)", ptr->h.all, ptr->v.all); \
	/* update de and dh */ \
	de = _add_n(de, _load_adjh(self->scv)); \
	nvec_t te = _max_n(de, t); \
	ptr->e.mask = _mask_n(_eq_n(te, de)); \
	de = _add_n(te, dh); \
	dh = _add_n(dh, t); \
	/* update df and dv */ \
	df = _add_n(df, _load_adjv(self->scv)); \
	nvec_t tf = _max_n(df, t); \
	ptr->f.mask = _mask_n(_eq_n(tf, df)); \
	df = _sub_n(tf, dv); \
	t = _sub_n(dv, t); \
	ptr++; \
	dv = dh; dh = t; \
	_print_n(_add_n(dh, _load_ofsh(self->scv))); \
	_print_n(_add_n(dv, _load_ofsv(self->scv))); \
	_print_n(_sub_n(_sub_n(de, dv), _load_adjh(self->scv))); \
	_print_n(_sub_n(_add_n(df, dh), _load_adjv(self->scv))); \
}
#else /* MODEL == COMBINED */
#define _fill_body() { \
	register nvec_t t = _match_n(_loadu_n(aptr), _loadu_n(bptr)); \
	_print_n(_loadu_n(aptr)); _print_n(_loadu_n(bptr)); \
	t = _shuf_n(_load_sb(self->scv), t); \
	t = _max_n(de, t); \
	t = _max_n(df, t); \
	ptr->h.mask = _mask_n(_eq_n(t, de)); \
	ptr->v.mask = _mask_n(_eq_n(t, df)); \
	debug("mask(%x, %x)", ptr->h.all, ptr->v.all); \
	/* update de and dh */ \
	de = _add_n(de, _load_adjh(self->scv)); \
	nvec_t te = _max_n(de, t); \
	ptr->e.mask = _mask_n(_eq_n(te, de)); \
	de = _add_n(te, dh); \
	dh = _add_n(dh, t); \
	/* update df and dv */ \
	df = _add_n(df, _load_adjv(self->scv)); \
	nvec_t tf = _max_n(df, t); \
	ptr->f.mask = _mask_n(_eq_n(tf, df)); \
	df = _sub_n(tf, dv); \
	t = _sub_n(dv, t); \
	ptr++; \
	dv = dh; dh = t; \
	_print_n(_add_n(dh, _load_ofsh(self->scv))); \
	_print_n(_add_n(dv, _load_ofsv(self->scv))); \
	_print_n(_sub_n(_sub_n(de, dv), _load_adjh(self->scv))); \
	_print_n(_sub_n(_add_n(df, dh), _load_adjv(self->scv))); \
}
#endif /* MODEL */

/**
 * @macro _fill_update_delta
 * @brief update small delta vector and max vector
 */
#define _fill_update_delta(_op_add, _op_subs, _vector, _offset, _sign) { \
	nvec_t _t = _add_n(_vector, _offset); \
	delta = _op_add(delta, _t); \
	drop = _op_subs(drop, _t); \
	_dir_update(dir, _vector, _sign); \
	_print_n(drop); \
	_print_w(_add_w(_load_w(&self->w.r.md), _add_w(_cvt_n_w(delta), _set_w(self->w.r.tail->offset + self->w.r.ofsd - 128)))); \
	_print_w(_add_w(_add_w(_load_w(&self->w.r.md), _cvt_n_w(delta)), _add_w(_cvt_n_w(drop), _set_w(self->w.r.tail->offset + self->w.r.ofsd)))); \
}

/**
 * @macro _fill_right, _fill_down
 * @brief wrapper of _fill_body and _fill_update_delta
 */
#define _fill_right_update_ptr() { \
	aptr--;				/* increment sequence buffer pointer */ \
}
#define _fill_right_windback_ptr() { \
	aptr++; \
}
#if MODEL == LINEAR
#define _fill_right() { \
	dh = _bsl_n(dh, 1);	/* shift left dh */ \
	_fill_body();		/* update vectors */ \
	_fill_update_delta(_add_n, _subs_n, dh, _load_ofsh(self->scv), 1); \
}
#else	/* AFFINE and COMBINED */
#define _fill_right() { \
	dh = _bsl_n(dh, 1);	/* shift left dh */ \
	df = _bsl_n(df, 1);	/* shift left df */ \
	_fill_body();		/* update vectors */ \
	_fill_update_delta(_sub_n, _adds_n, dh, _load_ofsh(self->scv), -1); \
}
#endif /* MODEL */
#define _fill_down_update_ptr() { \
	bptr++;				/* increment sequence buffer pointer */ \
}
#define _fill_down_windback_ptr() { \
	bptr--; \
}
#if MODEL == LINEAR
#define _fill_down() { \
	dv = _bsr_n(dv, 1);	/* shift right dv */ \
	_fill_body();		/* update vectors */ \
	_fill_update_delta(_add_n, _subs_n, dv, _load_ofsv(self->scv), 1); \
}
#else	/* AFFINE and COMBINED */
#define _fill_down() { \
	dv = _bsr_n(dv, 1);	/* shift right dv */ \
	de = _bsr_n(de, 1);	/* shift right de */ \
	_fill_body();		/* update vectors */ \
	_fill_update_delta(_add_n, _subs_n, dv, _load_ofsv(self->scv), 1); \
}
#endif /* MODEL */

/**
 * @macro _fill_store_context
 * @brief store vectors at the end of the block
 */
#define _fill_store_context_intl(_blk) ({ \
	/* store direction array */ \
	_dir_save(_blk, dir); \
	/* update xdrop status and offsets */ \
	int8_t _xstat = (_blk)->xstat = (self->tx - _ext_n(drop, BW/2)) & 0x80; \
	int32_t cofs = _ext_n(delta, BW/2); \
	/* store cnt */ \
	int32_t acnt = _rd_bufa(self, 0, BW) - aptr; \
	int32_t bcnt = bptr - _rd_bufb(self, 0, BW); \
	(_blk)->acnt = acnt; (_blk)->bcnt = bcnt; \
	/* write back local working buffers */ \
	self->w.r.ofsd += cofs; self->w.r.aridx -= acnt; self->w.r.bridx -= bcnt; \
	/* update max and middle vectors in the working buffer */ \
	nvec_t prev_drop = _load_n(&self->w.r.xd); \
	_store_n(&self->w.r.xd, drop);		/* save max delta vector */ \
	_print_n(prev_drop); _print_n(_add_n(drop, delta)); \
	(_blk)->max_mask = ((nvec_masku_t){ \
		.mask = _mask_n(_gt_n(_add_n(drop, delta), prev_drop)) \
	}).all; \
	debug("update_mask(%llx)", (uint64_t)(_blk)->max_mask); \
	/* update middle delta vector */ \
	wvec_t md = _load_w(&self->w.r.md); \
	md = _add_w(md, _cvt_n_w(_sub_n(delta, _set_n(cofs)))); \
	_store_w(&self->w.r.md, md); \
	_xstat; \
})
#if MODEL == LINEAR
#define _fill_store_context(_blk) ({ \
	_storeu_n((_blk)->diff.dh, dh); _print_n(dh); \
	_storeu_n((_blk)->diff.dv, dv); _print_n(dv); \
	_fill_store_context_intl(_blk); \
})
#else	/* AFFINE and COMBINED */
#define _fill_store_context(_blk) ({ \
	_storeu_n((_blk)->diff.dh, dh); _print_n(dh); \
	_storeu_n((_blk)->diff.dv, dv); _print_n(dv); \
	_storeu_n((_blk)->diff.de, de); _print_n(de); \
	_storeu_n((_blk)->diff.df, df); _print_n(df); \
	_fill_store_context_intl(_blk); \
})
#endif

/**
 * @fn fill_bulk_test_idx
 * @brief returns negative if ij-bound (for the bulk fill) is invaded
 */
static _force_inline
int64_t fill_bulk_test_idx(
	struct gaba_dp_context_s const *self)
{
	debug("test(%lld, %lld, %lld), len(%u, %u, %u)",
		(int64_t)self->w.r.aridx - BW,
		(int64_t)self->w.r.bridx - BW,
		(int64_t)self->w.r.pridx - BLK,
		self->w.r.aridx, self->w.r.bridx, self->w.r.pridx);
	#define _test(_label, _ofs)	( (int64_t)self->w.r._label - (int64_t)(_ofs) )
	return(_test(aridx, BW) | _test(bridx, BW) | _test(pridx, BLK));
	#undef _test
}

/**
 * @fn fill_cap_test_idx
 * @brief returns negative if ij-bound (for the cap fill) is invaded
 */
#define _fill_cap_test_idx_init() \
	uint8_t const *alim = _rd_bufa(self, self->w.r.aridx, BW); \
	uint8_t const *blim = _rd_bufb(self, self->w.r.bridx, BW); \
	uint8_t const *plim = blim - (ptrdiff_t)alim + (ptrdiff_t)self->w.r.pridx;
#define _fill_cap_test_idx() ({ \
	debug("arem(%zd), brem(%zd), prem(%zd)", aptr - alim, blim - bptr, plim - bptr + aptr); \
	((int64_t)aptr - (int64_t)alim) | ((int64_t)blim - (int64_t)bptr) | ((int64_t)plim - (int64_t)bptr + (int64_t)aptr); \
})

/**
 * @fn fill_bulk_block
 * @brief fill a block, returns negative value if xdrop detected
 */
static _force_inline
void fill_bulk_block(
	struct gaba_dp_context_s *self,
	struct gaba_block_s *blk)
{
	/* fetch sequence */
	fill_fetch_core(self, (blk - 1)->acnt, BLK, (blk - 1)->bcnt, BLK);

	/* load vectors onto registers */
	debug("blk(%p)", blk);
	_fill_load_context(blk);
	/**
	 * @macro _fill_block
	 * @brief unit unrolled fill-in loop
	 */
	#define _fill_block(_direction, _label, _jump_to) { \
		_dir_fetch(dir); \
		if(_unlikely(!_dir_is_##_direction(dir))) { \
			goto _fill_##_jump_to; \
		} \
		_fill_##_label: \
		_fill_##_direction##_update_ptr(); \
		_fill_##_direction(); \
		if(--i == 0) { break; } \
	}

	/* update diff vectors */
	int64_t i = BLK;
	while(1) {					/* 4x unrolled loop */
		_fill_block(down, d1, r1);
		_fill_block(right, r1, d2);
		_fill_block(down, d2, r2);
		_fill_block(right, r2, d1);
	}

	/* store vectors */
	self->w.r.pridx -= BLK;
	_fill_store_context(blk);
	return;
}

/**
 * @fn fill_bulk_k_blocks
 * @brief fill <cnt> contiguous blocks without ij-bound test
 */
static _force_inline
struct gaba_block_s *fill_bulk_k_blocks(
	struct gaba_dp_context_s *self,
	struct gaba_block_s *blk,
	uint64_t cnt)
{
	/* bulk fill loop, first check termination */
	struct gaba_block_s *tblk = blk + cnt;
	while((blk->xstat | (ptrdiff_t)(tblk - blk)) > 0) {
		/* bulk fill */
		debug("blk(%p)", blk + 1);
		fill_bulk_block(self, ++blk);
	}

	/* fix status flag (move MSb to TERM) and pridx */
	blk->xstat = blk->xstat < 0 ? TERM : ((blk->xstat & ~STAT_MASK) | CONT);
	debug("return, blk(%p), xstat(%x), pridx(%u)", blk, blk->xstat, self->w.r.pridx);
	return(blk);
}

/**
 * @fn fill_bulk_seq_bounded
 * @brief fill blocks with ij-bound test
 */
static _force_inline
struct gaba_block_s *fill_bulk_seq_bounded(
	struct gaba_dp_context_s *self,
	struct gaba_block_s *blk)
{
	/* bulk fill loop, first check termination */
	while((blk->xstat | fill_bulk_test_idx(self)) >= 0) {
		debug("blk(%p)", blk + 1);
		fill_bulk_block(self, ++blk);
	}

	/* fix status flag (move MSb to TERM) */
	blk->xstat = blk->xstat < 0 ? TERM : ((blk->xstat & ~STAT_MASK) | CONT);
	debug("return, blk(%p), xstat(%x)", blk, blk->xstat);
	return(blk);
}

/**
 * @fn fill_cap_seq_bounded
 * @brief fill blocks with cap test
 */
static _force_inline
struct gaba_block_s *fill_cap_seq_bounded(
	struct gaba_dp_context_s *self,
	struct gaba_block_s *blk)
{
	#define _fill_cap_seq_bounded_core(_dir) { \
		/* update sequence coordinate and then check term */ \
		_fill_##_dir##_update_ptr(); \
		if(_fill_cap_test_idx() < 0) { \
			_fill_##_dir##_windback_ptr(); \
			_dir_windback(dir); \
			break; \
		} \
		_fill_##_dir();		/* update band */ \
	}

	debug("blk(%p)", blk);
	while(blk->xstat >= 0) {
		/* fetch sequence */
		fill_cap_fetch(self, ++blk);
		_fill_cap_test_idx_init();
		_fill_load_context(blk);			/* contains ptr as struct gaba_mask_pair_s *ptr = blk->mask; */

		/* update diff vectors */
		struct gaba_mask_pair_s *tptr = &blk->mask[BLK];
		while(ptr < tptr) {					/* ptr is automatically incremented in _fill_right() or _fill_down() */
			_dir_fetch(dir);				/* determine direction */
			if(_dir_is_right(dir)) {
				_fill_cap_seq_bounded_core(right);
			} else {
				_fill_cap_seq_bounded_core(down);
			}
		}

		uint64_t i = ptr - blk->mask;		/* calc filled count */
		self->w.r.pridx -= i;				/* update remaining p-length */
		_dir_adjust_remainder(dir, i);		/* adjust dir remainder */
		_fill_store_context(blk);			/* store mask and vectors */
		if(i != BLK) { blk -= i == 0; break; }/* break if not filled full length */
	}

	/* fix status flag (move MSb to TERM) */
	blk->xstat = blk->xstat < 0 ? TERM : ((blk->xstat & ~STAT_MASK) | UPDATE);
	debug("return, blk(%p), xstat(%x)", blk, blk->xstat);
	return(blk);
}

/**
 * @fn max_blocks_mem
 * @brief calculate maximum number of blocks to be filled (bounded by stack size)
 */
static _force_inline
uint64_t max_blocks_mem(
	struct gaba_dp_context_s const *self)
{
	uint64_t mem_size = self->stack.end - self->stack.top;
	uint64_t blk_cnt = mem_size / sizeof(struct gaba_block_s);
	debug("calc_max_block_mem, stack_top(%p), stack_end(%p), mem_size(%llu), cnt(%llu)",
		self->stack.top, self->stack.end, mem_size, (blk_cnt > 3) ? (blk_cnt - 3) : 0);
	return(((blk_cnt > 3) ? blk_cnt : 3) - 3);
}

/**
 * @fn max_blocks_idx
 * @brief calc max #expected blocks from remaining seq lengths,
 * used to determine #blocks which can be filled without mem boundary check.
 */
static _force_inline
uint64_t max_blocks_idx(
	struct gaba_dp_context_s const *self)
{
	uint64_t p = MIN2(self->w.r.aridx, self->w.r.bridx);
	return(MIN2(2*p + p/2, self->w.r.pridx) / BLK + 1);
}

/**
 * @fn min_blocks_idx
 * @brief calc min #expected blocks from remaining seq lengths,
 * #blocks filled without seq boundary check.
 */
static _force_inline
uint64_t min_blocks_idx(
	struct gaba_dp_context_s const *self)
{
	uint64_t p = MIN2(self->w.r.aridx, self->w.r.bridx);
	return(MIN2(p + p/2, self->w.r.pridx) / BLK);
}

/**
 * @fn fill_seq_bounded
 * @brief fill blocks with seq bound tests (without mem test), adding head and tail
 */
static _force_inline
struct gaba_block_s *fill_seq_bounded(
	struct gaba_dp_context_s *self,
	struct gaba_block_s *blk)
{
	uint64_t cnt;						/* #blocks filled in bulk */
	debug("blk(%p), cnt(%llu)", blk, min_blocks_idx(self));
	while((cnt = min_blocks_idx(self)) > MIN_BULK_BLOCKS) {
		/* bulk fill without ij-bound test */
		debug("fill predetd blocks");
		if(((blk = fill_bulk_k_blocks(self, blk, cnt))->xstat & STAT_MASK) != CONT) {
			debug("term detected, xstat(%x)", blk->xstat);
			return(blk);				/* xdrop termination detected, skip cap */
		}
	}

	/* bulk fill with ij-bound test */
	if(((blk = fill_bulk_seq_bounded(self, blk))->xstat & STAT_MASK) != CONT) {
		debug("term detected, blk(%p), xstat(%x)", blk, blk->xstat);
		return(blk);					/* xdrop termination detected, skip cap */
	}

	/* cap fill (with p-bound test) */
	return(fill_cap_seq_bounded(self, blk));
}

/**
 * @fn fill_section_seq_bounded
 * @brief fill dp matrix inside section pairs
 */
static _force_inline
struct gaba_block_s *fill_section_seq_bounded(
	struct gaba_dp_context_s *self,
	struct gaba_block_s *blk)
{
	/* extra large bulk fill (with stack allocation) */
	uint64_t mem_cnt, seq_cnt;			/* #blocks filled in bulk */
	while((mem_cnt = max_blocks_mem(self)) < (seq_cnt = max_blocks_idx(self))) {
		debug("mem_cnt(%llu), seq_cnt(%llu)", mem_cnt, seq_cnt);

		/* here mem limit reaches first (seq sections too long), so fill without seq test */
		if((mem_cnt = MIN2(mem_cnt, min_blocks_idx(self))) > MIN_BULK_BLOCKS) {
			debug("mem bounded fill");
			if(((blk = fill_bulk_k_blocks(self, blk, mem_cnt))->xstat & STAT_MASK) != CONT) {
				return(blk);
			}
		}

		/* memory ran out: malloc a next stack and create a new phantom head */
		debug("add stack");
		if(gaba_dp_add_stack(self, 0) != 0) { return(NULL); }
		blk = fill_create_phantom(self, blk);
	}

	/* bulk fill with seq bound check */
	return(fill_seq_bounded(self, blk));
}

/**
 * @fn gaba_dp_fill_root
 *
 * @brief build_root API
 */
struct gaba_fill_s *_export(gaba_dp_fill_root)(
	struct gaba_dp_context_s *self,
	struct gaba_section_s const *a,
	uint32_t apos,
	struct gaba_section_s const *b,
	uint32_t bpos,
	uint32_t pridx)
{
	/* restore dp context pointer by adding offset */
	self = _restore_dp_context(self);

	/* load sections and extract the last block pointer */
	v2i32_t pos = _seta_v2i32(bpos, apos), len = _seta_v2i32(b->len, a->len);
	struct gaba_block_s *blk = fill_load_tail(
		self, _root(self), a, b,
		_cvt_v2i32_u64(_sub_v2i32(len, pos)),
		pridx == 0 ? UINT32_MAX : pridx /* UINT32_MAX */
	);

	/* init fetch */
	if(fill_init_fetch(self, blk, _root(self)->f.ppos) < GP_ROOT) {
		return(_fill(fill_create_tail(self, blk)));
	}

	/* init fetch done, issue ungapped extension here if filter is needed */
	/* fill blocks then create a tail cap */
	return(_fill(fill_create_tail(self,
		fill_section_seq_bounded(self, blk)
	)));
}

/**
 * @fn gaba_dp_fill
 *
 * @brief fill API
 */
struct gaba_fill_s *_export(gaba_dp_fill)(
	struct gaba_dp_context_s *self,
	struct gaba_fill_s const *fill,
	struct gaba_section_s const *a,
	struct gaba_section_s const *b,
	uint32_t pridx)
{
	self = _restore_dp_context(self);

	/* load sections and extract the last block pointer */
	_print_v2i32(_load_v2i32(&_tail(fill)->aridx));
	_print_v2i32(_load_v2i32(&_tail(fill)->asridx));
	struct gaba_block_s *blk = fill_load_tail(
		self, _tail(fill), a, b,
		_loadu_u64(&_tail(fill)->aridx),
		pridx == 0 ? _tail(fill)->pridx : pridx /* UINT32_MAX */
	);

	/* check if still in the init (head) state */
	if(_tail(fill)->f.ppos < GP_ROOT) {
		if(fill_init_fetch(self, blk, _tail(fill)->f.ppos) < GP_ROOT) {
			return(_fill(fill_create_tail(self, blk)));
		}
		/* init fetch done, issue ungapped extension here if filter is needed */
	}

	/* fill blocks then create a tail cap */
	return(_fill(fill_create_tail(self,
		fill_section_seq_bounded(self, blk)
	)));
}


/* merge bands */
/**
 * @fn gaba_dp_merge
 *
 * @brief merge API (merge two bands)
 */
struct gaba_fill_s *_export(gaba_dp_merge)(
	gaba_dp_t *dp,
	struct gaba_fill_s const *fill1,
	struct gaba_fill_s const *fill2,
	int32_t qdiff)
{
	/**
	 * FIXME: NOTE:
	 *
	 * This function creates a new tail object from the two input
	 * tail objects (fill1 and fill2) by ``merging'' them. The two
	 * tails must be aligned on the same anti-diagonal line, that is,
	 * they must have the same (local) p-coordinates. The last
	 * argument qdiff is the distance between the two tails,
	 * defined as fill2->qpos - fill1->qpos. The absolute distance,
	 * |qdiff|, must not be larger than BW because non-overlapping
	 * vectors cannot be merged. (undefined return value is reasonable
	 * for now, I think.)
	 *
	 * Internal structure of the tail object:
	 * Two structs, gaba_phantom_s and gaba_joint_tail_s, adjoins
	 * on memory. Each fill pointer points to the `struct gaba_fill_s f`
	 * member. So pointers to the two structs (objects) can be restored
	 * by subtracting a certain offsets.
	 *
	 * Merging algorithm (tentative):
	 * The middle_delta array contains the actual (offsetted) cell values
	 * of the last vector of the band. So the two vectors can be merged
	 * by max-ing cell values at each lane. The second last vectors are
	 * also merged by max-ing each value. The actual values of the second
	 * last vector is restored by subtracting the difference vector
	 * (either dv or dh, depends on the last advancing direction) from the
	 * middle delta vector.
	 *
	 *	struct gaba_phantom_s {
	 *		struct gaba_diff_vec_s diff;	// calculated from the two merged middle delta vectors (the last and the second last vector)
	 *		uint32_t reserved;
	 *		int8_t acc, xstat;				// difference of the two edge cell values, MERGE_HEAD
	 *		int8_t acnt, bcnt;				// 0, 0
	 *		struct gaba_block_s const *blk;	// NULL
	 *	};
	 *	struct gaba_joint_tail_s {
	 *		struct gaba_char_vec_s ch;		// copied from the input tails
	 *		struct gaba_drop_s xd;			// MAX(fill1->xd, fill2->xd)
	 *		struct gaba_middle_delta_s md;	// MAX(fill1->md + fill1->offset, fill2->md + fill2->offset) - new_offset
	 *		int8_t qdiff[2], unused[2];		// TBD
	 *		uint32_t pridx;					// MIN(fill1->pridx, fill2->pridx)
	 *		uint32_t aridx, bridx;			// TBD
	 *		uint32_t asridx, bsridx;		// aridx, bridx above
	 *		int64_t offset;					// new_offset (TBD)
	 *		struct gaba_fill_s f;			// (max, stat, scnt, ppos) = (calculated from the new_offset, copied? (TBD), MAX(fill1->scnt, fill2->scnt), MAX(fill1->ppos, fill2->ppos))
	 *		struct gaba_joint_tail_s const *tail;// left NULL
	 *		union {
	 *			struct gaba_section_pair_s s;// unused
	 *			struct gaba_tail_pair_s t;	// (tail[2], tail_idx_mask[2]) = ({ fill1, fill2 }, { max_mask1, max_mask2 })
	 *		} u;
	 *	};
	 */
	return(NULL);
}


/* max score search functions */
/**
 * @fn leaf_load_max_mask
 */
static _force_inline
uint64_t leaf_load_max_mask(
	struct gaba_dp_context_s *self,
	struct gaba_joint_tail_s const *tail)
{
	debug("ppos(%lld), scnt(%d), offset(%lld)", tail->f.ppos, tail->f.scnt, tail->offset);

	/* load max vector, create mask */
	nvec_t drop = _loadu_n(tail->xd.drop);
	wvec_t md = _loadu_w(tail->md.delta);
	uint64_t max_mask = ((nvec_masku_t){
		.mask = _mask_w(_eq_w(
			_set_w(tail->f.max - tail->offset),
			_add_w(md, _cvt_n_w(drop))
		))
	}).all;
	debug("max_mask(%llx)", max_mask);
	_print_w(_set_w(tail->f.max - tail->offset));
	_print_w(_add_w(md, _cvt_n_w(drop)));
	return(max_mask);
}

/**
 * @fn leaf_detect_pos
 * @brief refill block to obtain cell-wise update-masks
 */
static _force_inline
void leaf_detect_pos(
	struct gaba_dp_context_s *self,
	struct gaba_block_s const *blk,
	uint64_t max_mask)
{
	#define _fill_block_leaf(_m) { \
		_dir_fetch(dir); \
		if(_dir_is_right(dir)) { \
			_fill_right_update_ptr(); \
			_fill_right(); \
		} else { \
			_fill_down_update_ptr(); \
			_fill_down(); \
		} \
		_m++->mask = _mask_n(_gt_n(delta, max)); \
		max = _max_n(delta, max); \
		debug("mask(%x)", ((nvec_masku_t){ .mask = (_m - 1)->mask }).all); \
	}

	/* load contexts and overwrite max vector */
	nvec_masku_t mask_arr[BLK], *m = mask_arr;		/* cell-wise update-mask array */
	/* vectors on registers */ {
		_fill_load_context(blk);
		nvec_t max = delta;
		for(int64_t i = 0; i < blk->acnt + blk->bcnt; i++) {
			_fill_block_leaf(m);
		}
	}

	/* search max cell, probe from the tail to the head */
	while(m > mask_arr && (max_mask & ~(--m)->all) != 0) {
		debug("max_mask(%llx), m(%llx)", max_mask, (uint64_t)m->all);
		max_mask &= ~m->all;
	}
	debug("max_mask(%llx), m(%llx)", max_mask, (uint64_t)m->all);
	self->w.l.p = m - mask_arr;
	self->w.l.q = tzcnt(m->all & max_mask);
	debug("p(%u), q(%u)", self->w.l.p, self->w.l.q);
	return;
}

/**
 * @fn leaf_search
 * @brief returns resulting path length
 */
static _force_inline
uint64_t leaf_search(
	struct gaba_dp_context_s *self,
	struct gaba_joint_tail_s const *tail)
{
	/* load mask and block pointers */
	uint64_t max_mask = leaf_load_max_mask(self, tail);
	struct gaba_block_s const *b = _last_block(tail) + 1;
	debug("max_mask(%llx)", max_mask);

	/*
	 * iteratively clear lanes with longer paths;
	 * max_mask will be zero if the block contains the maximum scoring cell with the shortest path
	 */
	v2i32_t ridx = _load_v2i32(&tail->aridx);
	_print_v2i32(ridx);
	do {
		if((--b)->xstat & ROOT) { debug("reached root, xstat(%x)", b->xstat); return(0); }	/* actually unnecessary but placed as a sentinel */
		if(b->xstat & HEAD) { b = _last_phantom(b + 1)->blk; }

		/* first adjust ridx to the head of this block then test mask was updated in this block */
		v2i8_t cnt = _load_v2i8(&b->acnt);
		ridx = _add_v2i32(ridx, _cvt_v2i8_v2i32(cnt));
		_print_v2i32(_cvt_v2i8_v2i32(cnt)); _print_v2i32(ridx);
		debug("max_mask(%llx), update_mask(%llx)", max_mask, (uint64_t)b->max_mask);
	} while((max_mask & ~b->max_mask) && (max_mask &= ~b->max_mask));

	/* calc (p, q) coordinates from block */
	fill_restore_fetch(self, tail, b, ridx);		/* fetch from existing blocks for p-coordinate search */
	leaf_detect_pos(self, b, max_mask);				/* calc local p,q-coordinates */
	self->w.l.blk = b;								/* max detection finished and reader_work has released, save block pointer to writer_work */

	/* restore reverse indices */
	int64_t fcnt = self->w.l.p + 1;					/* #filled vectors */
	uint32_t dir_mask = _dir_load(self->w.l.blk, fcnt).mask;/* 1 for b-side extension, 0 for a-side extension */
	_print_v2i32(ridx);
	ridx = _sub_v2i32(ridx,
		_seta_v2i32(
			(0    + popcnt(dir_mask)) - (BW - self->w.l.q),	/* bcnt - bqdiff where popcnt(dir_mask) is #b-side extensions */
			(fcnt - popcnt(dir_mask)) - (1 + self->w.l.q)	/* acnt - aqdiff */
		)
	);
	_print_v2i32(ridx);

	/* increment by one to convert char index to grid index */
	v2i32_t gidx = _sub_v2i32(_set_v2i32(1), ridx);
	gidx = _add_v2i32(gidx, _load_v2i32(&tail->aridx));		/* compensate tail lengths */

	_store_v2i32(&self->w.l.agidx, gidx);			/* current gidx */
	_store_v2i32(&self->w.l.asgidx, gidx);			/* tail gidx (pos) */
	_print_v2i32(gidx);

	/* calc plen */
	v2i32_t eridx = _load_v2i32(&tail->aridx);
	v2i32_t rem = _sub_v2i32(ridx, eridx);
	_print_v2i32(eridx); _print_v2i32(rem);
	debug("path length: %lld", tail->f.ppos + (BW + 1) - _hi32(rem) - _lo32(rem));
	return(tail->f.ppos + (BW + 1) - _hi32(rem) - _lo32(rem));
}

/**
 * @fn gaba_dp_search_max
 */
struct gaba_pos_pair_s _export(gaba_dp_search_max)(
	struct gaba_dp_context_s *self,
	struct gaba_fill_s const *fill)
{
	self = _restore_dp_context(self);
	struct gaba_joint_tail_s const *tail = _tail(fill);
	leaf_search(self, tail);

	v2i32_t const v11 = _set_v2i32(1);
	v2i32_t const mask = _seta_v2i32(GABA_UPDATE_B, GABA_UPDATE_A);
	v2i32_t gidx = _load_v2i32(&self->w.l.agidx), acc = _zero_v2i32();
	while(_test_v2i32(_gt_v2i32(v11, gidx), v11)) {
		/* accumulate advanced lengths */
		v2i32_t adv = _sub_v2i32(_load_v2i32(&tail->aridx), _load_v2i32(&tail->asridx));
		acc = _add_v2i32(acc, adv);

		/* load update flag and calculate update mask */
		v2i32_t flag = _set_v2i32(tail->f.stat);
		v2i32_t update = _eq_v2i32(_and_v2i32(flag, mask), mask);

		/* add advanced lengths */
		gidx = _add_v2i32(gidx, _and_v2i32(update, adv));
		adv = _andn_v2i32(update, adv);

		/* fetch prev */
		tail = tail->tail;
	}
	struct gaba_pos_pair_s pos;
	_store_v2i32(&pos, _sub_v2i32(gidx, v11));
	return(pos);
}


/* path trace functions */
/**
 * @fn trace_reload_section
 * @brief reload section and adjust gidx, pass 0 for section a or 1 for section b
 */
static _force_inline
void trace_reload_section(
	struct gaba_dp_context_s *self,
	uint64_t i)
{
	#define _r(_x, _idx)		( (&(_x))[(_idx)] )
	static uint32_t const mask[2] = { GABA_UPDATE_A, GABA_UPDATE_B };

	debug("load section %s, idx(%d), len(%d)",
		i == 0 ? "a" : "b", _r(self->w.l.agidx, i), _r(_r(self->w.l.atail, i)->u.s.alen, i));

	/* load tail pointer (must be inited with leaf tail) */
	struct gaba_joint_tail_s const *tail = _r(self->w.l.atail, i);
	int32_t gidx = _r(self->w.l.agidx, i);
	debug("base gidx(%d)", gidx);

	while(gidx <= 0) {
		do {
			gidx += _r(tail->asridx, i) - _r(tail->aridx, i);
			debug("add len(%d), adv(%d), gidx(%d), stat(%x)", _r(tail->u.s.alen, i), _r(tail->asridx, i) - _r(tail->aridx, i), gidx, tail->f.stat);
			tail = tail->tail;
		} while((tail->f.stat & mask[i]) == 0);
	}

	/* reload finished, store section info */
	_r(self->w.l.atail, i) = tail;		/* FIXME: is this correct?? */
	_r(self->w.l.aid, i) = _r(tail->u.s.aid, i);

	_r(self->w.l.agidx, i) = gidx;
	_r(self->w.l.asgidx, i) = gidx;
	return;

	#undef _r
}

/**
 * @enum ts_*
 */
enum { ts_d = 0, ts_v0, ts_v1, ts_h0, ts_h1 };

/**
 * @macro _trace_inc_*
 * @brief increment gap counters
 */
#define _trace_inc_gi()					{ gc = _sub_v2i32(gc, v01); }
#define _trace_inc_ge()					{ gc = _sub_v2i32(gc, v10); }
#define _trace_inc_gf()					{ gc = _sub_v2i32(gc, v00); }

/**
 * @macro _trace_*_*_test_index
 * @brief test if path reached a section boundary
 */
#define _trace_bulk_v_test_index()		( 0 )
#define _trace_bulk_d_test_index()		( 0 )
#define _trace_bulk_h_test_index()		( 0 )
#define _trace_tail_v_test_index()		( _test_v2i32(gidx, v10) /* bgidx == 0 */ )
#define _trace_tail_d_test_index()		( !_test_v2i32(_eq_v2i32(gidx, v00), v11) /* agidx == 0 || bgidx == 0 */ )
#define _trace_tail_h_test_index()		( _test_v2i32(gidx, v01) /* agidx == 0 */ )

/**
 * @macro _trace_*_*_update_index
 * @brief update indices by one
 */
#define _trace_bulk_v_update_index()	;
#define _trace_bulk_h_update_index()	;
#define _trace_tail_v_update_index()	{ gidx = _add_v2i32(gidx, v10); _print_v2i32(gidx); }
#define _trace_tail_h_update_index()	{ gidx = _add_v2i32(gidx, v01); _print_v2i32(gidx); }

/**
 * @macro _trace_test_*
 * @brief test mask
 */
#if MODEL == LINEAR
#define _trace_test_diag_h()			( (mask->h.all>>q) & 0x01 )
#define _trace_test_diag_v()			( (mask->v.all>>q) & 0x01 )
#define _trace_test_gap_h()				( (mask->h.all>>q) & 0x01 )
#define _trace_test_gap_v()				( (mask->v.all>>q) & 0x01 )
#else /* MODEL == AFFINE */
#define _trace_test_diag_h()			( (mask->h.all>>q) & 0x01 )
#define _trace_test_diag_v()			( (mask->v.all>>q) & 0x01 )
#define _trace_test_gap_h()				( (mask->e.all>>q) & 0x01 )
#define _trace_test_gap_v()				( (mask->f.all>>q) & 0x01 )
#endif

/**
 * @macro _trace_*_update_path_q
 */
#define _trace_h_update_path_q() { \
	path_array = path_array<<1; \
	mask--; \
	q += _dir_mask_is_down(dir_mask); \
	/*fprintf(stderr, "h: q(%u)\n", q);*/ \
	_dir_mask_windback(dir_mask); \
}
#define _trace_v_update_path_q() { \
	path_array = (path_array<<1) | 0x01; \
	mask--; \
	q += _dir_mask_is_down(dir_mask) - 1; \
	/*fprintf(stderr, "v: q(%u)\n", q);*/ \
	_dir_mask_windback(dir_mask); \
}

/**
 * @macro _trace_load_block_rem
 * @brief reload mask pointer and direction mask, and adjust path offset
 */
#define _trace_load_block_rem(_cnt) ({ \
	debug("ofs(%llu), cnt(%llu), path(%p), ofs(%llu)", \
		ofs, (uint64_t)_cnt, path - (ofs < (_cnt)), (ofs - (_cnt)) & (BLK - 1)); \
	path -= ofs < (_cnt); \
	ofs = (ofs - (_cnt)) & (BLK - 1); \
	_dir_mask_load(blk, (_cnt)); \
})

/**
 * @macro _trace_reload_tail
 * @brief reload tail, issued at each band-segment boundaries
 */
#define _trace_reload_tail(t, _vec_idx) { \
	/* store path (offset will be adjusted afterwards) */ \
	_storeu_u64(path, path_array<<ofs); \
	/* reload block pointer */ \
	blk = _last_phantom(blk)->blk; \
	while(blk->xstat & MERGE_HEAD) { \
		struct gaba_joint_tail_s *tail = (void *)(blk + 1); \
		uint64_t _tail_idx = (tail->u.t.tail_idx_mask[_vec_idx]>>q) & 0x01; \
		struct gaba_joint_tail_s const *prev = tail->u.t.tail[_tail_idx]; \
		q += tail->qdiff[_tail_idx]; blk = _last_block(prev);	/* adjust q, restore block pointer */ \
		tail->tail = prev;	/* save prev pointer for use in section boundary detection */ \
	} \
	/* reload dir and mask pointer, adjust path offset */ \
	uint64_t _cnt = blk->acnt + blk->bcnt; \
	mask = &blk->mask[_cnt - 1]; \
	dir_mask = _trace_load_block_rem(_cnt); \
	debug("reload tail, path_array(%llx), blk(%p), idx(%llu), mask(%x)", path_array, blk, _cnt - 1, dir_mask); \
}

/**
 * @macro _trace_reload_block
 * @brief reload block for bulk trace
 */
#define _trace_reload_block() { \
	/* store path (bulk, offset does not change here) */ \
	_storeu_u64(path, path_array<<ofs); path--; \
	/* reload mask and mask pointer */ \
	mask = &(--blk)->mask[BLK - 1]; \
	dir_mask = _dir_mask_load(blk, BLK); \
	debug("reload block, path_array(%llx), blk(%p), mask(%x)", path_array, blk, dir_mask); \
}

/**
 * @macro _trace_test_bulk
 * @brief update gidx and test if section boundary can be found in the next block.
 * adjust gidx to the next base and return 0 if bulk loop can be continued,
 * otherwise gidx will not updated. must be called just after reload_ptr or reload_tail.
 */
#if 1
#define _trace_test_bulk() ({ \
	v2i8_t _cnt = _load_v2i8(&blk->acnt); \
	v2i32_t _gidx = _sub_v2i32(gidx, _cvt_v2i8_v2i32(_cnt)); \
	debug("test bulk, _cnt(%d, %d), _gidx(%d, %d), test(%d)", \
		_hi32(_cvt_v2i8_v2i32(_cnt)), _lo32(_cvt_v2i8_v2i32(_cnt)), _hi32(_gidx), _lo32(_gidx), \
		_test_v2i32(_gt_v2i32(bw, _gidx), v11) ? 1 : 0); \
	_test_v2i32(_gt_v2i32(bw, _gidx), v11) ? (gidx = _gidx, 1) : 0;	/* agidx >= BW && bgidx >= BW */ \
})
#else
#define _trace_test_bulk()	0
#endif

/**
 * @macro _trace_*_load
 * @brief set _state 0 when in diagonal loop, otherwise pass 1
 */
#define _trace_bulk_load_n(t, _state, _jump_to) { \
	if(_unlikely(mask < blk->mask)) { \
		_trace_reload_block(); \
		if(_unlikely(!_trace_test_bulk())) {	/* adjust gidx */ \
			gidx = _add_v2i32(gidx, _seta_v2i32(q - qsave, qsave - q)); \
			debug("jump to %s, adjust gidx, q(%d), prev_q(%d), adj(%d, %d), gidx(%u, %u)", #_jump_to, \
				q, self->w.l.q, q - qsave, qsave - q, _hi32(gidx), _lo32(gidx)); \
			goto _jump_to;						/* jump to tail loop */ \
		} \
	} \
}
#define _trace_tail_load_n(t, _state, _jump_to) { \
	if(_unlikely(mask < blk->mask)) { \
		debug("test ph(%p), xstat(%x), ppos(%llu), path(%p, %p), ofs(%u)", \
			_last_phantom(blk), _last_phantom(blk)->xstat, (path - self->w.l.aln->path) * 32 + ofs, path, self->w.l.aln->path, ofs); \
		if(_last_phantom(blk)->xstat & HEAD) {	/* head (phantom) block is marked 0x4000 */ \
			_trace_reload_tail(t, _state);		/* fetch the previous tail */ \
		} else { \
			/* load dir and update mask pointer */ \
			_trace_reload_block();				/* not reached the head yet */ \
			if(_trace_test_bulk()) {			/* adjust gidx */ \
				debug("save q(%d)", q); \
				qsave = q; \
				goto _jump_to;					/* dispatch again (bulk loop) */ \
			} \
		} \
	} \
}

/**
 * @fn trace_core
 */
static _force_inline
void trace_core(
	struct gaba_dp_context_s *self)
{
	#define _pop_vector(_c, _l, _state, _jump_to) { \
		debug("go %s (%s, %s), dir(%x), mask_h(%x), mask_v(%x), p(%lld), q(%d), ptr(%p), path_array(%llx)", \
			#_l, #_c, #_jump_to, dir_mask, mask->h.all, mask->v.all, (int64_t)(mask - blk->mask), (int32_t)q, mask, path_array); \
		_trace_##_c##_##_l##_update_index(); \
		_trace_##_l##_update_path_q(); \
		_trace_##_c##_load_n(t, _state, _jump_to); \
	}
	#define _trace_gap_loop(t, _c, _n, _l) { \
		_trace_##_c##_##_l##_head: \
			if(_trace_##_c##_##_l##_test_index()) { self->w.l.state = ts_##_l##0; goto _trace_term; } \
			_trace_inc_gi();		/* increment #gap regions at the head */ \
			_pop_vector(_c, _l, 1, _trace_##_n##_##_l##_head); \
			while(1) { \
		_trace_##_c##_##_l##_mid: \
				if(_trace_test_gap_##_l() == 0) { goto _trace_##_c##_d_head; } \
				if(_trace_##_c##_##_l##_test_index()) { self->w.l.state = ts_##_l##1; goto _trace_term; } \
				_trace_inc_ge();	/* increment #gap bases on every iter */ \
				_pop_vector(_c, _l, 1, _trace_##_n##_##_l##_head); \
			} \
	}
	#define _trace_diag_loop(t, _c, _n) { \
		while(1) { \
		_trace_##_c##_d_head: \
			if(_trace_test_diag_h() != 0) { goto _trace_##_c##_h_head; } \
			if(_trace_##_c##_d_test_index()) { self->w.l.state = ts_d; goto _trace_term; } \
			_pop_vector(_c, h, 0, _trace_##_n##_d_mid); \
		_trace_##_c##_d_mid: \
			_pop_vector(_c, v, 0, _trace_##_n##_d_tail); \
		_trace_##_c##_d_tail: \
			if(_trace_test_diag_v() != 0) { goto _trace_##_c##_v_head; } \
		} \
	}

	/* constants for agidx and bgidx end detection */
	v2i32_t const v00 = _seta_v2i32(0, 0);
	v2i32_t const v01 = _seta_v2i32(0, -1);
	v2i32_t const v10 = _seta_v2i32(-1, 0);
	v2i32_t const v11 = _seta_v2i32(-1, -1);
	v2i32_t const bw = _set_v2i32(BW);

	/* gap counts; #gap regions in lower and #gap bases in higher */
	register v2i32_t gc = _load_v2i32(&self->w.l.a.gicnt);

	/* load path array, adjust path offset to align the head of the current block */
	uint64_t ofs = self->w.l.ofs;	/* global p-coordinate */
	uint32_t *path = self->w.l.path;
	uint64_t path_array = _loadu_u64(path)>>ofs;

	/* load pointers and coordinates */
	struct gaba_block_s const *blk = self->w.l.blk;
	struct gaba_mask_pair_s const *mask = &blk->mask[self->w.l.p];
	uint32_t q = self->w.l.q, qsave = q;
	uint32_t dir_mask = _trace_load_block_rem(self->w.l.p + 1);

	/* load grid indices; assigned for each of N+1 boundaries of length-N sequence */
	register v2i32_t gidx = _load_v2i32(&self->w.l.agidx);
	_print_v2i32(gidx);

	/* dispatcher: jump to the last state */
	switch(self->w.l.state) {
		case ts_d:  goto _trace_tail_d_head;
		case ts_v0: goto _trace_tail_v_head;
		case ts_v1: goto _trace_tail_v_mid;
		case ts_h0: goto _trace_tail_h_head;
		case ts_h1: goto _trace_tail_h_mid;
	}

	/* tail loops */ {
		_trace_gap_loop(self, tail, bulk, v);	/* v is the beginning state */
		_trace_diag_loop(self, tail, bulk);
		_trace_gap_loop(self, tail, bulk, h);
	}
	/* bulk loops */ {
		_trace_gap_loop(self, bulk, tail, v);
		_trace_diag_loop(self, bulk, tail);
		_trace_gap_loop(self, bulk, tail, h);
	}

_trace_term:;
	/* reached a boundary of sections, compensate ofs */
	uint64_t rem = mask - blk->mask + 1;
	debug("rem(%llu), path(%p), ofs(%llu)", rem, path + (ofs + rem >= BLK), (ofs + rem) & (BLK - 1));
	path += (ofs + rem) >= BLK;
	ofs = (ofs + rem) & (BLK - 1);
	_storeu_u64(path, path_array<<ofs);

	/* save gap counts */
	_store_v2i32(&self->w.l.a.gicnt, gc);

	/* store pointers and coordinates */
	self->w.l.ofs = ofs;			/* global p-coordinate */
	self->w.l.path = path;
	self->w.l.blk = blk;
	self->w.l.p = mask - blk->mask;	/* local p-coordinate */
	self->w.l.q = q;				/* q coordinate */
	_store_v2i32(&self->w.l.agidx, gidx);
	_print_v2i32(gidx);
	return;
}

/**
 * @fn trace_push_segment
 */
static _force_inline
void trace_push_segment(
	struct gaba_dp_context_s *self)
{
	/* windback pointer */
	self->w.l.a.slen++;
	self->w.l.a.seg--;

	/* calc ppos */
	uint64_t ppos = (self->w.l.path - self->w.l.aln->path) * 32 + self->w.l.ofs;
	debug("ppos(%llu), path(%p, %p), ofs(%u)", ppos, self->w.l.path, self->w.l.aln->path, self->w.l.ofs);

	/* load section info */
	v2i32_t gidx = _load_v2i32(&self->w.l.agidx);
	v2i32_t sgidx = _load_v2i32(&self->w.l.asgidx);
	v2i32_t id = _load_v2i32(&self->w.l.aid);
	_print_v2i32(gidx); _print_v2i32(sgidx); _print_v2i32(id);

	/* store section info */
	_store_v2i32(&self->w.l.a.seg->apos, gidx);
	_store_v2i32(&self->w.l.a.seg->alen, _sub_v2i32(sgidx, gidx));
	_store_v2i32(&self->w.l.a.seg->aid, id);
	self->w.l.a.seg->ppos = ppos;

	/* update rsgidx */
	_store_v2i32(&self->w.l.asgidx, gidx);
	return;
}

/**
 * @fn trace_init
 */
static _force_inline
void trace_init(
	struct gaba_dp_context_s *self,
	struct gaba_joint_tail_s const *tail,
	struct gaba_alloc_s const *alloc,
	uint64_t plen)
{
	/* store tail pointers for sequence segment tracking */
	self->w.l.atail = tail;
	self->w.l.btail = tail;

	/* malloc container */
	uint64_t sn = 2 * tail->f.scnt, pn = (plen + 31) / 32 + 2;
	uint64_t size = (
		  sizeof(struct gaba_alignment_s)				/* base */
		+ sizeof(uint32_t) * _roundup(pn, 8)			/* path array and its margin */
		+ sizeof(struct gaba_segment_s) * sn			/* segment array */
		+ self->head_margin + self->tail_margin			/* head and tail margins */
	);

	/* save aln pointer and memory management stuffs to working buffer */
	self->w.l.aln = alloc->lmalloc(alloc->opaque, size) + self->head_margin;
	self->w.l.a.opaque = alloc->opaque;					/* save opaque pointer */
	self->w.l.a.lfree = alloc->lfree;
	self->w.l.a.head_margin = self->head_margin;		/* required when free the object */

	/* use gaba_alignment_s buffer instead in the traceback loop */
	self->w.l.a.plen = plen;
	self->w.l.a.score = tail->f.max;					/* just copy */
	self->w.l.a.mcnt = 0;
	self->w.l.a.xcnt = 0;
	self->w.l.a.gicnt = 0;
	self->w.l.a.gecnt = 0;

	/* section */
	self->w.l.a.slen = 0;
	self->w.l.a.seg = (sn - 1) + (struct gaba_segment_s *)(self->w.l.aln->path + _roundup(pn, 8)),

	/* clear state */
	self->w.l.state = ts_d;

	/* store block and coordinates */
	self->w.l.ofs = plen & (32 - 1);
	self->w.l.path = self->w.l.aln->path + plen / 32;

	/* clear array */
	self->w.l.path[0] = 0x01<<self->w.l.ofs;
	self->w.l.path[1] = 0;
	return;
}

/**
 * @fn trace_body
 * @brief plen = 0 generates an alignment object with no section (not NULL object).
 * may fail when path got lost out of the band and returns NULL object
 */
static _force_inline
struct gaba_alignment_s *trace_body(
	struct gaba_dp_context_s *self,
	struct gaba_joint_tail_s const *tail,
	struct gaba_alloc_s const *alloc,
	uint64_t plen)
{
	/* create alignment object */
	trace_init(self, tail, alloc, plen);

	/* blockwise traceback loop, until ppos reaches the root */
	while(self->w.l.path + self->w.l.ofs > self->w.l.aln->path) {	/* !(self->w.l.path == self->w.l.aln->path && self->w.l.ofs == 0) */
		/* update section info */
		debug("gidx(%d, %d)", self->w.l.bgidx, self->w.l.agidx);
		if((int32_t)self->w.l.agidx <= 0) { trace_reload_section(self, 0); }
		if((int32_t)self->w.l.bgidx <= 0) { trace_reload_section(self, 1); }

		/* fragment trace: q must be inside [0, BW) */
		trace_core(self);
		debug("p(%d), q(%d)", self->w.l.p, self->w.l.q);
		if(_unlikely(self->w.l.q >= BW)) {
			/* out of band: abort */
			self->w.l.a.lfree(self->w.l.a.opaque,
				(void *)((uint8_t *)self->w.l.aln - self->w.l.a.head_margin)
			);
			return(NULL);
		}

		/* push section info to segment array */
		trace_push_segment(self);
	}

	/* calc mismatch and match counts */
	self->w.l.a.xcnt = 0;
	/*
	self->w.l.a.xcnt = ((int64_t)self->m * ((self->w.l.a.plen - self->w.l.a.gecnt)>>1)
		- (self->w.l.a.score - (int64_t)self->gi * self->w.l.a.gicnt - (int64_t)self->ge * self->w.l.a.gecnt)
	) / (self->m - self->x);
	self->w.l.a.mcnt = ((self->w.l.a.plen - self->w.l.a.gecnt)>>1) - self->w.l.a.xcnt;
	debug("sc(%d, %d, %d, %d), score(%lld), plen(%lld), gic(%u), gec(%u), dcnt(%u), esc(%lld), div(%d), xcnt(%u)",
		self->m, self->x, self->gi, self->ge, self->w.l.a.score,
		self->w.l.a.plen, self->w.l.a.gicnt, self->w.l.a.gecnt,
		(uint32_t)(self->w.l.a.plen - self->w.l.a.gecnt)>>1,
		self->w.l.a.score - (int64_t)self->gi * self->w.l.a.gicnt - (int64_t)self->ge * self->w.l.a.gecnt,
		self->m - self->x, self->w.l.a.xcnt);
	*/

	/* copy */
	_memcpy_blk_ua(self->w.l.aln, &self->w.l.a, sizeof(struct gaba_alignment_s));
	return(self->w.l.aln);
}

/**
 * @fn gaba_dp_trace
 */
struct gaba_alignment_s *_export(gaba_dp_trace)(
	struct gaba_dp_context_s *self,
	struct gaba_fill_s const *fill,
	struct gaba_alloc_s const *alloc)
{
	/* restore dp context pointer by adding offset */
	self = _restore_dp_context(self);

	/* restore default alloc if NULL */
	struct gaba_alloc_s const default_alloc = {
		.opaque = (void *)self,
		.lmalloc = (gaba_lmalloc_t)gaba_dp_malloc,
		.lfree = (gaba_lfree_t)gaba_dp_free
	};
	alloc = (alloc == NULL) ? &default_alloc : alloc;

	/* search and trace */
	return(trace_body(self, _tail(fill), alloc,
		fill->ppos < GP_ROOT ? 0 : leaf_search(self, _tail(fill))
	));
}


/**
 * @fn gaba_dp_res_free
 */
void _export(gaba_dp_res_free)(
	struct gaba_alignment_s *aln)
{
	struct gaba_aln_intl_s *a = (struct gaba_aln_intl_s *)aln;
	debug("free mem, ptr(%p), lmm(%p)", (void *)a - a->head_margin, a->opaque);
	a->lfree(a->opaque, (void *)((uint8_t *)a - a->head_margin));
	return;
}

/**
 * @fn parse_load_uint64
 */
static _force_inline
uint64_t parse_load_uint64(
	uint64_t const *ptr,
	int64_t pos)
{
	int64_t rem = pos & 63;
	uint64_t a = (ptr[pos>>6]>>rem) | ((ptr[(pos>>6) + 1]<<(63 - rem))<<1);
	return(a);
}

/**
 * @fn parse_dump_match_string
 */
static _force_inline
int64_t parse_dump_match_string(
	char *buf,
	int64_t len)
{
	if(len < 64) {
		static uint8_t const conv[64] = {
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
			0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
			0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
			0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
			0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
			0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
			0x60, 0x61, 0x62, 0x63
		};
		char *p = buf;
		*p = (conv[len]>>4) + '0'; p += (conv[len] & 0xf0) != 0;
		*p++ = (conv[len] & 0x0f) + '0';
		*p++ = 'M';
		return(p - buf);
	} else {
		int64_t adv;
		uint8_t b[16] = { 'M', '0' }, *p = &b[1];
		while(len != 0) { *p++ = (len % 10) + '0'; len /= 10; }
		for(p -= (p != &b[1]), adv = (int64_t)((ptrdiff_t)(p - b)) + 1; p >= b; p--) { *buf++ = *p; }
		return(adv);
	}
}

/**
 * @fn parse_dump_gap_string
 */
static _force_inline
int64_t parse_dump_gap_string(
	char *buf,
	int64_t len,
	char ch)
{
	if(len < 64) {
		static uint8_t const conv[64] = {
			0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
			0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
			0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
			0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
			0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
			0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
			0x60, 0x61, 0x62, 0x63
		};
		char *p = buf;
		*p = (conv[len]>>4) + '0'; p += (conv[len] & 0xf0) != 0;
		*p++ = (conv[len] & 0x0f) + '0';
		*p++ = ch;
		return(p - buf);
	} else {
		int64_t adv;
		uint8_t b[16] = { ch, '0' }, *p = &b[1];
		while(len != 0) { *p++ = (len % 10) + '0'; len /= 10; }
		for(p -= (p != &b[1]), adv = (int64_t)((ptrdiff_t)(p - b)) + 1; p >= b; p--) { *buf++ = *p; }
		return(adv);
	}
}

/**
 * @macro _parse_count_match_forward, _parse_count_gap_forward
 */
#define _parse_count_match_forward(_arr) ({ \
	tzcnt((_arr) ^ 0x5555555555555555); \
})
#define _parse_count_gap_forward(_arr) ({ \
	uint64_t _a = (_arr); \
	uint64_t mask = 0ULL - (_a & 0x01); \
	uint64_t gc = tzcnt(_a ^ mask) + (uint64_t)mask; \
	gc; \
})

/**
 * @fn gaba_dp_print_cigar_forward
 * @brief parse path string and print cigar to file
 */
uint64_t _export(gaba_dp_print_cigar_forward)(
	gaba_dp_printer_t printer,
	void *fp,
	uint32_t const *path,
	uint32_t offset,
	uint32_t len)
{
	uint64_t clen = 0;

	/* convert path to uint64_t pointer */
	uint64_t const *p = (uint64_t const *)((uint64_t)path & ~(sizeof(uint64_t) - 1));
	uint64_t lim = offset + (((uint64_t)path & sizeof(uint32_t)) ? 32 : 0) + len;
	uint64_t ridx = len;

	while(1) {
		uint64_t rsidx = ridx;
		while(1) {
			uint64_t m = _parse_count_match_forward(parse_load_uint64(p, lim - ridx));
			uint64_t a = MIN2(m, ridx) & ~0x01;
			ridx -= a;
			ZCNT_RESULT uint64_t c = a;
			if(c < 64) { break; }
		}
		uint64_t m = (rsidx - ridx)>>1;
		if(m > 0) {
			clen += printer(fp, m, 'M');
		}
		if(ridx == 0) { break; }

		uint64_t arr;
		uint64_t g = MIN2(
			_parse_count_gap_forward(arr = parse_load_uint64(p, lim - ridx)),
			ridx);
		if(g > 0) {
			clen += printer(fp, g, 'D' + ((char)(0ULL - (arr & 0x01)) & ('I' - 'D')));
		}
		if((ridx -= g) <= 1) { break; }
	}
	return(clen);
}

/**
 * @fn gaba_dp_dump_cigar_forward
 * @brief parse path string and store cigar to buffer
 */
uint64_t _export(gaba_dp_dump_cigar_forward)(
	char *buf,
	uint64_t buf_size,
	uint32_t const *path,
	uint32_t offset,
	uint32_t len)
{
	uint64_t const filled_len_margin = 5;
	char *b = buf, *blim = buf + buf_size - filled_len_margin;

	/* convert path to uint64_t pointer */
	uint64_t const *p = (uint64_t const *)((uint64_t)path & ~(sizeof(uint64_t) - 1));
	uint64_t lim = offset + (((uint64_t)path & sizeof(uint32_t)) ? 32 : 0) + len;
	uint64_t ridx = len;

	while(1) {
		uint64_t rsidx = ridx;
		while(1) {
			uint64_t m = _parse_count_match_forward(parse_load_uint64(p, lim - ridx));
			uint64_t a = MIN2(m, ridx) & ~0x01;
			ridx -= a;
			ZCNT_RESULT uint64_t c = a;
			if(c < 64) { break; }
		}
		uint64_t m = (rsidx - ridx)>>1;
		if(m > 0) {
			b += parse_dump_match_string(b, m);
		}
		if(ridx == 0 || b > blim) { break; }

		uint64_t arr;
		uint64_t g = MIN2(
			_parse_count_gap_forward(arr = parse_load_uint64(p, lim - ridx)),
			ridx);
		if(g > 0) {
			b += parse_dump_gap_string(b, g, 'D' + ((char)(0ULL - (arr & 0x01)) & ('I' - 'D')));
		}
		if((ridx -= g) <= 1 || b > blim) { break; }
	}
	*b = '\0';
	return(b - buf);
}

/**
 * @macro _parse_count_match_reverse, _parse_count_gap_reverse
 */
#define _parse_count_match_reverse(_arr) ({ \
	lzcnt((_arr) ^ 0x5555555555555555); \
})
#define _parse_count_gap_reverse(_arr) ({ \
	uint64_t _a = (_arr); \
	uint64_t mask = (uint64_t)(((int64_t)_a)>>63); \
	uint64_t gc = lzcnt(_a ^ mask) - ((int64_t)mask + 1); \
	gc; \
})

/**
 * @fn gaba_dp_print_cigar_reverse
 * @brief parse path string and print cigar to file
 */
uint64_t _export(gaba_dp_print_cigar_reverse)(
	gaba_dp_printer_t printer,
	void *fp,
	uint32_t const *path,
	uint32_t offset,
	uint32_t len)
{
	int64_t clen = 0;

	/* convert path to uint64_t pointer */
	uint64_t const *p = (uint64_t const *)((uint64_t)path & ~(sizeof(uint64_t) - 1));
	uint64_t ofs = (int64_t)offset + (((uint64_t)path & sizeof(uint32_t)) ? -32 : -64);
	uint64_t idx = len;

	while(1) {
		uint64_t sidx = idx;
		while(1) {
			uint64_t m = _parse_count_match_reverse(parse_load_uint64(p, idx + ofs));
			uint64_t a = MIN2(m, idx) & ~0x01;
			idx -= a;
			if(a < 64) { break; }
		}
		uint64_t m = (sidx - idx)>>1;
		if(m > 0) {
			clen += printer(fp, m, 'M');
		}
		if(idx == 0) { break; }

		uint64_t arr;
		uint64_t g = MIN2(
			_parse_count_gap_reverse(arr = parse_load_uint64(p, idx + ofs)),
			idx);
		if(g > 0) {
			clen += printer(fp, g, 'D' + ((char)(((int64_t)arr)>>63) & ('I' - 'D')));
		}
		if((idx -= g) <= 1) { break; }
	}
	return(clen);
}

/**
 * @fn gaba_dp_dump_cigar_reverse
 * @brief parse path string and store cigar to buffer
 */
uint64_t _export(gaba_dp_dump_cigar_reverse)(
	char *buf,
	uint64_t buf_size,
	uint32_t const *path,
	uint32_t offset,
	uint32_t len)
{
	uint64_t const filled_len_margin = 5;
	char *b = buf, *blim = buf + buf_size - filled_len_margin;

	/* convert path to uint64_t pointer */
	uint64_t const *p = (uint64_t const *)((uint64_t)path & ~(sizeof(uint64_t) - 1));
	uint64_t ofs = (int64_t)offset + (((uint64_t)path & sizeof(uint32_t)) ? -32 : -64);
	uint64_t idx = len;

	while(1) {
		uint64_t sidx = idx;
		while(1) {
			uint64_t m = _parse_count_match_reverse(parse_load_uint64(p, idx + ofs));
			uint64_t a = MIN2(m, idx) & ~0x01;
			idx -= a;
			if(a < 64) { break; }
		}
		uint64_t m = (sidx - idx)>>1;
		if(m > 0) {
			b += parse_dump_match_string(b, m);
		}
		if(idx == 0 || b > blim) { break; }

		uint64_t arr;
		uint64_t g = MIN2(
			_parse_count_gap_reverse(arr = parse_load_uint64(p, idx + ofs)),
			idx);
		if(g > 0) {
			b += parse_dump_gap_string(b, g, 'D' + ((char)(((int64_t)arr)>>63) & ('I' - 'D')));
		}
		if((idx -= g) <= 1 || b > blim) { break; }
	}
	*b = '\0';
	return(b - buf);
}

/**
 * @fn gaba_init_restore_default
 */
static _force_inline
void gaba_init_restore_default(
	struct gaba_params_s *p)
{
	#define restore(_name, _default) { \
		p->_name = ((uint64_t)(p->_name) == 0) ? (_default) : (p->_name); \
	}
	uint32_t zm = ((v32i8_masku_t){
		.mask = _mask_v32i8(_eq_v32i8(_loadu_v32i8(p->score_matrix), _zero_v32i8()))
	}).all;
	if((zm & 0xfffff) == 0) {
		/* score matrix for erroneous long reads */
		v16i8_t sc = _seta_v16i8(
			1, -1, -1, -1,
			-1, 1, -1, -1,
			-1, -1, 1, -1,
			-1, -1, -1, 1
		);
		_storeu_v16i8(p->score_matrix, sc);
		/* affine gap penalty */
		p->gi = 1; p->ge = 1; p->gfa = 0; p->gfb = 0;
	}
	restore(xdrop, 			50);
	restore(filter_thresh,	0);					/* disable filter */
	restore(head_margin,	0);
	restore(tail_margin,	0);
	return;
}

/**
 * @fn gaba_init_check_score
 * @brief return non-zero if the score is not applicable
 */
static _force_inline
int64_t gaba_init_check_score(
	struct gaba_params_s const *p)
{
	if(_max_match(p) > 7) { return(-1); }
	if(p->ge <= 0) { return(-1); }
	if(p->gi < 0) { return(-1); }
	if(p->gfa < 0 || (p->gfa != 0 && p->gfa <= p->ge)) { return(-1); }
	if(p->gfb < 0 || (p->gfb != 0 && p->gfb <= p->ge)) { return(-1); }
	if((p->gfa == 0) ^ (p->gfb == 0)) { return(-1); }
	for(int32_t i = 0; i < BW/2; i++) {
		int32_t t1 = _ofs_h(p) + _gap_h(p, i*2 + 1) - _gap_h(p, i*2);
		int32_t t2 = _ofs_h(p) + (_max_match(p) + _gap_v(p, i*2 + 1)) - _gap_v(p, (i + 1) * 2);
		int32_t t3 = _ofs_v(p) + (_max_match(p) + _gap_h(p, i*2 + 1)) - _gap_v(p, (i + 1) * 2);
		int32_t t4 = _ofs_v(p) + _gap_h(p, i*2 + 1) - _gap_h(p, i*2);

		if(MAX4(t1, t2, t3, t4) > 127) { return(-1); }
		if(MIN4(t2, t2, t3, t4) < 0) { return(-1); }
	}
	return(0);
}

/**
 * @fn gaba_init_score_vector
 */
static _force_inline
struct gaba_score_vec_s gaba_init_score_vector(
	struct gaba_params_s const *p)
{
	v16i8_t scv = _loadu_v16i8(p->score_matrix);
	int8_t ge = -p->ge, gi = -p->gi;
	struct gaba_score_vec_s sc __attribute__(( aligned(MEM_ALIGN_SIZE) ));

	/* score matrices */
	#if BIT == 4
		int8_t m = _hmax_v16i8(scv);
		int8_t x = _hmax_v16i8(_sub_v16i8(_zero_v16i8(), scv));
		scv = _add_v16i8(_bsl_v16i8(_set_v16i8(m + x), 1), _set_v16i8(-x));
	#endif
	_store_sb(sc, _add_v16i8(scv, _set_v16i8(-2 * (ge + gi))));

	/* gap penalties */
	#if MODEL == LINEAR
		_store_adjh(sc, 0, 0, ge + gi, ge + gi);
		_store_adjv(sc, 0, 0, ge + gi, ge + gi);
		_store_ofsh(sc, 0, 0, ge + gi, ge + gi);
		_store_ofsv(sc, 0, 0, ge + gi, ge + gi);
	#elif MODEL == AFFINE
		_store_adjh(sc, -gi, -gi, -(ge + gi), ge + gi);
		_store_adjv(sc, -gi, -gi, -(ge + gi), ge + gi);
		_store_ofsh(sc, -gi, -gi, -(ge + gi), ge + gi);
		_store_ofsv(sc, -gi, -gi, -(ge + gi), ge + gi);
	#else	/* COMBINED */
		_store_adjh(sc, -gi, -gi, -(ge + gi), ge + gi);
		_store_adjv(sc, -gi, -gi, -(ge + gi), ge + gi);
		_store_ofsh(sc, -gi, -gi, -(ge + gi), ge + gi);
		_store_ofsv(sc, -gi, -gi, -(ge + gi), ge + gi);
	#endif
	return(sc);
}

/**
 * @fn gaba_init_middle_delta
 */
static _force_inline
struct gaba_middle_delta_s gaba_init_middle_delta(
	struct gaba_params_s const *p)
{
	struct gaba_middle_delta_s md __attribute__(( aligned(MEM_ALIGN_SIZE) ));
	for(int i = 0; i < BW/2; i++) {
		md.delta[BW/2 - 1 - i] = -(i + 1) * _max_match(p) + _gap_h(p, i*2 + 1);
		md.delta[BW/2     + i] = -(i + 1) * _max_match(p) + _gap_v(p, i*2 + 1);
	}
	_print_w(_load_w(&md));
	return(md);
}

/**
 * @fn gaba_init_diff_vectors
 * @detail
 * dH[i, j] = S[i, j] - S[i - 1, j]
 * dV[i, j] = S[i, j] - S[i, j - 1]
 * dE[i, j] = E[i, j] - S[i, j]
 * dF[i, j] = F[i, j] - S[i, j]
 */
static _force_inline
struct gaba_diff_vec_s gaba_init_diff_vectors(
	struct gaba_params_s const *p)
{
	struct gaba_diff_vec_s diff __attribute__(( aligned(MEM_ALIGN_SIZE) ));
	/**
	 * dh: dH[i, j] - ge
	 * dv: dV[i, j] - ge
	 * de: dE[i, j] + gi + dV[i, j] - ge
	 * df: dF[i, j] + gi + dH[i, j] - ge
	 */
	for(int i = 0; i < BW/2; i++) {
		diff.dh[BW/2 - 1 - i] = _ofs_h(p) + _gap_h(p, i*2 + 1) - _gap_h(p, i*2);
		diff.dh[BW/2     + i] = _ofs_h(p) + _max_match(p) + _gap_v(p, i*2 + 1) - _gap_v(p, (i + 1) * 2);
		diff.dv[BW/2 - 1 - i] = _ofs_v(p) + _max_match(p) + _gap_h(p, i*2 + 1) - _gap_v(p, (i + 1) * 2);
		diff.dv[BW/2     + i] = _ofs_v(p) + _gap_v(p, i*2 + 1) - _gap_v(p, i*2);
	#if MODEL == AFFINE || MODEL == COMBINED
		diff.de[BW/2 - 1 - i] = _ofs_e(p) + diff.dv[BW/2 - 1 - i];
		diff.de[BW/2     + i] = _ofs_e(p) + diff.dv[BW/2     + i] - p->gi;
		diff.df[BW/2 - 1 - i] = _ofs_f(p) + diff.dh[BW/2 - 1 - i] - p->gi;
		diff.df[BW/2     + i] = _ofs_f(p) + diff.dh[BW/2     + i];
	#endif
	}
	#if MODEL == AFFINE || MODEL == COMBINED
		/* negate dh for affine and combined */
		_store_n(&diff.dh, _sub_n(_zero_n(), _load_n(&diff.dh)));
	#endif
	return(diff);
}

/**
 * @fn gaba_init_phantom
 * @brief phantom block at root
 */
static _force_inline
void gaba_init_phantom(
	struct gaba_root_block_s *ph,
	struct gaba_params_s *p)
{
	/* 192 bytes are reserved for phantom block */
	*_last_phantom(&ph->tail) = (struct gaba_phantom_s){
		.acc = 0,  .xstat = ROOT | UPDATE | HEAD,
		.acnt = 0, .bcnt = 0,
		.blk = NULL,
		.diff = gaba_init_diff_vectors(p)
	};

	/* fill root tail object */
	ph->tail = (struct gaba_joint_tail_s){
		/* fill object: coordinate and status */
		.f = {
			.max = 0,
			.stat = CONT | GABA_UPDATE_A | GABA_UPDATE_B,
			.scnt = 0,
			.ppos = GP_INIT - BW,
		},

		/* section info */
		.tail = NULL,
		.aridx = 0,  .bridx = 0,
		.asridx = 0, .bsridx = 0,
		.u = { .s = { 0 } },

		/* score and vectors */
		.offset = 0,
		.ch.w = { [0] = _max_match_base(p), [BW-1] = _max_match_base(p)<<4 },
		.xd.drop = { [BW / 2] = _max_match(p) - _gap_v(p, 1) },
		.md = gaba_init_middle_delta(p)
	};

	/* add offsets */
	ph->tail.offset += 128;
	_store_n(&ph->tail.xd.drop,
		_add_n(_load_n(&ph->tail.xd.drop), _set_n(-128))
	);
	return;
}

/**
 * @fn gaba_init_dp_context
 */
static _force_inline
void gaba_init_dp_context(
	struct gaba_context_s *ctx,
	struct gaba_params_s *p)
{
	/* initialize template, see also: "loaded on init" mark and GABA_DP_CONTEXT_LOAD_OFFSET */
	ctx->dp = (struct gaba_dp_context_s){
		/* score vectors */
		.scv = gaba_init_score_vector(p),
		.tx = p->xdrop - 128,
		.tf = p->filter_thresh,

		/* input and output options */
		.head_margin = _roundup(p->head_margin, MEM_ALIGN_SIZE),
		.tail_margin = _roundup(p->tail_margin, MEM_ALIGN_SIZE),

		/* pointers to root vectors */
		.root = {
			[_dp_ctx_index(16)] = &_proot(ctx, 16)->tail,
			[_dp_ctx_index(32)] = &_proot(ctx, 32)->tail,
			[_dp_ctx_index(64)] = &_proot(ctx, 64)->tail
		}
	};
	return;
}

/**
 * @fn gaba_init
 * @brief params can be NULL (default params for noisy long reads are loaded).
 */
gaba_t *_export(gaba_init)(
	struct gaba_params_s const *p)
{
	/* restore defaults and check sanity */
	struct gaba_params_s pi = (p != NULL		/* copy params to local stack to modify it afterwards */
		? *p
		: (struct gaba_params_s){ { 0 }, 0 }
	);
	gaba_init_restore_default(&pi);				/* restore defaults */
	if(gaba_init_check_score(&pi) != 0) {		/* check the scores are applicable */
		return(NULL);
	}

	/* malloc gaba_context_s */
	struct gaba_context_s *ctx = NULL;
	if(pi.reserved == NULL) {
		if((ctx = gaba_malloc(sizeof(struct gaba_context_s))) == NULL) {
			return(NULL);
		}
		gaba_init_dp_context(ctx, &pi);
	} else {
		/* fill phantom objects of existing dp context */
		ctx = (struct gaba_context_s *)pi.reserved;
	}

	/* load default phantom objects */
	gaba_init_phantom(_proot(ctx, BW), &pi);
	return((gaba_t *)ctx);
}

/**
 * @fn gaba_clean
 */
void _export(gaba_clean)(
	struct gaba_context_s *ctx)
{
	gaba_free(ctx);
	return;
}

/**
 * @fn gaba_dp_init
 */
struct gaba_dp_context_s *_export(gaba_dp_init)(
	struct gaba_context_s const *ctx,
	uint8_t const *alim,
	uint8_t const *blim)
{
	/* malloc stack memory */
	struct gaba_dp_context_s *self = gaba_malloc(sizeof(struct gaba_dp_context_s) + MEM_INIT_SIZE);
	if(self == NULL) {
		debug("failed to malloc memory");
		return(NULL);
	}

	/* add offset */
	self = _restore_dp_context_global(self);

	/* copy template */
	_memcpy_blk_aa(
		(uint8_t *)self + GABA_DP_CONTEXT_LOAD_OFFSET,
		(uint8_t *)&ctx->dp + GABA_DP_CONTEXT_LOAD_OFFSET,
		GABA_DP_CONTEXT_LOAD_SIZE
	);

	/* init stack pointers */
	self->stack.mem = &self->mem;
	self->stack.top = (uint8_t *)(self + 1);
	self->stack.end = (uint8_t *)self + MEM_INIT_SIZE;

	/* init mem object */
	self->mem = (struct gaba_mem_block_s){
		.next = NULL,
		.prev = NULL,
		.size = MEM_INIT_SIZE
	};

	/* init seq lims */
	self->alim = alim;
	self->blim = blim;

	/* return offsetted pointer */
	return(_export_dp_context(self));
}

/**
 * @fn gaba_dp_add_stack
 * @brief returns zero when succeeded
 */
static _force_inline
int64_t gaba_dp_add_stack(
	struct gaba_dp_context_s *self,
	uint64_t size)
{
	if(self->stack.mem->next == NULL) {
		/* current stack is the tail of the memory block chain, add new block */
		size = MAX2(
			size + _roundup(sizeof(struct gaba_mem_block_s), MEM_ALIGN_SIZE),
			2 * self->stack.mem->size
		);
		struct gaba_mem_block_s *mem = gaba_malloc(size);
		if(mem == NULL) { return(-1); }

		/* link new node to the tail of the current chain */
		self->stack.mem->next = mem;

		/* initialize the new memory block */
		mem->next = NULL;
		mem->size = size;
	}
	/* follow a forward link and init stack pointers */
	self->stack.mem = self->stack.mem->next;
	self->stack.top = (uint8_t *)_roundup((uintptr_t)(self->stack.mem + 1), MEM_ALIGN_SIZE);
	self->stack.end = (uint8_t *)self->stack.mem + self->stack.mem->size;
	return(0);
}

/**
 * @fn gaba_dp_flush
 */
void _export(gaba_dp_flush)(
	struct gaba_dp_context_s *self,
	uint8_t const *alim,
	uint8_t const *blim)
{
	/* restore dp context pointer by adding offset */
	self = _restore_dp_context(self);

	/* init seq lims */
	self->alim = alim;
	self->blim = blim;

	self->stack.mem = &self->mem;
	self->stack.top = (uint8_t *)_roundup((uintptr_t)(self->stack.mem + 1), MEM_ALIGN_SIZE);
	self->stack.end = (uint8_t *)self + self->mem.size;
	return;
}

/**
 * @fn gaba_dp_save_stack
 */
struct gaba_stack_s const *_export(gaba_dp_save_stack)(
	struct gaba_dp_context_s *self)
{
	self = _restore_dp_context(self);

	/* load stack content before gaba_stack_s object is allocated from it */
	struct gaba_stack_s save = self->stack;

	/* save the previously loaded content to the stack object */
	struct gaba_stack_s *stack = gaba_dp_malloc(self, sizeof(struct gaba_stack_s));
	*stack = save;
	debug("save stack(%p), (%p, %p, %p)", stack, stack->mem, stack->top, stack->end);
	return(stack);
}

/**
 * @fn gaba_dp_flush_stack
 */
void _export(gaba_dp_flush_stack)(
	struct gaba_dp_context_s *self,
	gaba_stack_t const *stack)
{
	if(stack == NULL) { return; }
	self = _restore_dp_context(self);
	self->stack = *stack;
	debug("restore stack, self(%p), mem(%p, %p, %p)", self, stack->mem, stack->top, stack->end);
	return;
}

/**
 * @fn gaba_dp_malloc
 */
static _force_inline
void *gaba_dp_malloc(
	struct gaba_dp_context_s *self,
	uint64_t size)
{
	/* roundup */
	size = _roundup(size, MEM_ALIGN_SIZE);

	/* malloc */
	debug("self(%p), stack_top(%p), size(%llu)", self, self->stack.top, size);
	if((uint64_t)(self->stack.end - self->stack.top) < size) {
		if(gaba_dp_add_stack(self, size) != 0) {
			return(NULL);
		}
		debug("stack.top(%p)", self->stack.top);
	}
	self->stack.top += size;
	return((void *)(self->stack.top - size));
}

/**
 * @fn gaba_dp_free
 * @brief do nothing
 */
static _force_inline
void gaba_dp_free(
	struct gaba_dp_context_s *self,
	void *ptr)
{
	return;
}

/**
 * @fn gaba_dp_clean
 */
void _export(gaba_dp_clean)(
	struct gaba_dp_context_s *self)
{
	if(self == NULL) {
		return;
	}

	/* restore dp context pointer by adding offset */
	self = _restore_dp_context(self);

	struct gaba_mem_block_s *m = self->mem.next;
	while(m != NULL) {
		struct gaba_mem_block_s *mnext = m->next;
		free(m); m = mnext;
	}
	gaba_free(_export_dp_context_global(self));
	return;
}

/* unittests */
#if UNITTEST == 1

#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * @fn unittest_build_context
 * @brief build context for unittest
 */
#if MODEL == LINEAR
static struct gaba_params_s const *unittest_default_params = GABA_PARAMS(
	.xdrop = 100,
	GABA_SCORE_SIMPLE(2, 3, 0, 6));
#else
static struct gaba_params_s const *unittest_default_params = GABA_PARAMS(
	.xdrop = 100,
	GABA_SCORE_SIMPLE(2, 3, 5, 1));
#endif
static
void *unittest_build_context(void *params)
{
	/* build context */
	gaba_t *ctx = _export(gaba_init)(unittest_default_params);
	return((void *)ctx);
}

/**
 * @fn unittest_clean_context
 */
static
void unittest_clean_context(void *ctx)
{
	_export(gaba_clean)((struct gaba_context_s *)ctx);
	return;
}

/**
 * misc macros and functions for assertion
 */
#define check_tail(_t, _max, _p, _psum, _scnt) ( \
	   (_t) != NULL \
	&& (_t)->max == (_max) \
	&& (_t)->ppos == (_psum) \
	&& (_t)->scnt == (_scnt) \
)
#define print_tail(_t) \
	"tail(%p), max(%lld), ppos(%lld), scnt(%u)", \
	(_t), (_t)->max, (_t)->ppos, (_t)->scnt
#define check_result(_r, _score, _xcnt, _plen, _slen, _rsidx, _rppos, _rapos, _rbpos) ( \
	   (_r) != NULL \
	&& (_r)->seg != NULL \
	&& (_r)->plen == (_plen) \
	&& (_r)->slen == (_slen) \
	&& (_r)->score == (_score) \
	&& (_r)->xcnt == (_xcnt) \
)
#define print_result(_r) \
	"res(%p), score(%lld), xcnt(%lld), plen(%u), slen(%u)", \
	(_r), (_r)->score, (_r)->xcnt, (_r)->plen, (_r)->slen

static
int check_path(
	struct gaba_alignment_s const *aln,
	char const *str)
{
	int64_t plen = aln->plen, slen = strlen(str);
	uint32_t const *p = aln->path;
	debug("%s", str);

	/* first check length */
	if(plen != slen) {
		debug("plen(%lld), slen(%lld)", plen, slen);
		return(0);
	}

	/* next compare encoded string (bit string) */
	while(plen > 0) {
		uint32_t array = 0;
		for(int64_t i = 0; i < 32; i++) {
			if(plen-- == 0) {
				array = (array>>(32 - i)) | ((uint64_t)0x01<<i);
				break;
			}
			array = (array>>1) | ((*str++ == 'D') ? 0x80000000 : 0);
			debug("%c, %x", str[-1], array);
		}
		debug("path(%x), array(%x)", *p, array);
		if(*p++ != array) {
			return(0);
		}
	}
	return(1);
}

static
int check_cigar(
	struct gaba_alignment_s const *aln,
	char const *cigar)
{
	char buf[1024];

	debug("path(%x), len(%lld)", aln->path[0], aln->plen);

	uint64_t l = _export(gaba_dp_dump_cigar_forward)(
		buf, 1024, aln->path, 0, aln->plen);

	debug("cigar(%s)", buf);

	/* first check length */
	if(strlen(cigar) != l) { return(0); }

	/* next compare cigar string */
	return((strcmp(buf, cigar) == 0) ? 1 : 0);
}

#define decode_path(_r) ({ \
	uint64_t plen = (_r)->plen, cnt = 0; \
	uint32_t const *path = (_r)->path; \
	uint32_t path_array = *path; \
	char *ptr = alloca(plen); \
	char *p = ptr; \
	while(plen-- > 0) { \
		*p++ = (path_array & 0x01) ? 'D' : 'R'; \
		path_array >>= 1; \
		if(++cnt == 32) { \
			path_array = *++path; \
			cnt = 0; \
		} \
	} \
	*p = '\0'; \
	ptr; \
})
#define print_path(_r)			"%s", decode_path(_r)
#define check_section(_s, _a, _apos, _alen, _b, _bpos, _blen, _ppos, _pl) ( \
	   (_s).aid == (_a).id \
	&& (_s).apos == (_apos) \
	&& (_s).alen == (_alen) \
	&& (_s).bid == (_b).id \
	&& (_s).bpos == (_bpos) \
	&& (_s).blen == (_blen) \
	&& (_s).ppos == (_ppos) \
	&& _plen(&(_s)) == (_pl) \
)
#define print_section(_s) \
	"a(%u), apos(%u), alen(%u), b(%u), bpos(%u), blen(%u), ppos(%u), plen(%u)", \
	(_s).aid, (_s).apos, (_s).alen, \
	(_s).bid, (_s).bpos, (_s).blen, \
	(_s).ppos, _plen(&(_s))

/* global configuration of the tests */
unittest_config(
	.name = "gaba",
	.init = unittest_build_context,
	.clean = unittest_clean_context
);

/**
 * check if gaba_init returns a valid pointer to a context
 */
unittest()
{
	struct gaba_context_s const *c = (struct gaba_context_s const *)gctx;
	assert(c != NULL, "%p", c);
}

/**
 * check if gaba_dp_init returns a vaild pointer to a dp context
 */
unittest()
{
	uint8_t const *lim = (uint8_t const *)0x800000000000;
	struct gaba_context_s const *c = (struct gaba_context_s const *)gctx;
	struct gaba_dp_context_s *d = _export(gaba_dp_init)(c, lim, lim);

	assert(d != NULL, "%p", d);
	_export(gaba_dp_clean)(d);
}

/* print_cigar test */
static
int ut_printer(
	void *pbuf,
	int64_t len,
	char c)
{
	char *buf = *((char **)pbuf);
	len = sprintf(buf, "%" PRId64 "%c", len, c);
	*((char **)pbuf) += len;
	return(len);
}

unittest()
{
	char *buf = (char *)malloc(16384);
	char *p = buf;

	#define _arr(...)		( (uint32_t const []){ 0, 0, __VA_ARGS__, 0, 0 } + 2 )
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0x55555555), 0, 32);
	assert(strcmp(buf, "16M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0x55555555, 0x55555555), 0, 64);
	assert(strcmp(buf, "32M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0x55555555, 0x55555555, 0x55555555, 0x55555555), 0, 128);
	assert(strcmp(buf, "64M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0x55550000, 0x55555555, 0x55555555, 0x55555555), 16, 112);
	assert(strcmp(buf, "56M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0x55555000, 0x55555555, 0x55555555, 0x55555555), 12, 116);
	assert(strcmp(buf, "58M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0x55), 0, 8);
	assert(strcmp(buf, "4M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0x55555000, 0x55555555, 0x55555555, 0x55), 12, 92);
	assert(strcmp(buf, "46M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0x55550555), 0, 32);
	assert(strcmp(buf, "6M4D8M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0x5555f555), 0, 32);
	assert(strcmp(buf, "6M4I8M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0xaaaa0555), 0, 33);
	assert(strcmp(buf, "6M5D8M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0xaaabf555), 0, 33);
	assert(strcmp(buf, "6M5I8M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0xaaabf555, 0xaaaa0556), 0, 65);
	assert(strcmp(buf, "6M5I8M1I5M5D8M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0xaaabf555, 0xaaaa0556, 0xaaaaaaaa), 0, 65);
	assert(strcmp(buf, "6M5I8M1I5M5D8M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_forward)(ut_printer, (void *)&p, _arr(0xaaabf554, 0xaaaa0556, 0xaaaaaaaa), 0, 65);
	assert(strcmp(buf, "2D5M5I8M1I5M5D8M") == 0, "%s", buf);

	#undef _arr
	free(buf);
}

unittest()
{
	char *buf = (char *)malloc(16384);
	char *p = buf;

	#define _arr(...)		( (uint32_t const []){ 0, 0, __VA_ARGS__, 0, 0 } + 2 )
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0x55555555), 0, 32);
	assert(strcmp(buf, "16M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0x55555555, 0x55555555), 0, 64);
	assert(strcmp(buf, "32M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0x55555555, 0x55555555, 0x55555555, 0x55555555), 0, 128);
	assert(strcmp(buf, "64M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0x55550000, 0x55555555, 0x55555555, 0x55555555), 16, 112);
	assert(strcmp(buf, "56M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0x55555000, 0x55555555, 0x55555555, 0x55555555), 12, 116);
	assert(strcmp(buf, "58M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0x55), 0, 8);
	assert(strcmp(buf, "4M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0x55555000, 0x55555555, 0x55555555, 0x55), 12, 92);
	assert(strcmp(buf, "46M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0x55550555), 0, 32);
	assert(strcmp(buf, "8M4D6M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0x5555f555), 0, 32);
	assert(strcmp(buf, "8M4I6M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0xaaaa0555), 0, 33);
	assert(strcmp(buf, "8M5D6M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0xaaabf555), 0, 33);
	assert(strcmp(buf, "8M5I6M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0xaaabf555, 0xaaaa0556), 0, 65);
	assert(strcmp(buf, "8M5D5M1I8M5I6M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0xaaabf555, 0xaaaa0556, 0xaaaaaaaa), 0, 65);
	assert(strcmp(buf, "8M5D5M1I8M5I6M") == 0, "%s", buf);

	p = buf;
	_export(gaba_dp_print_cigar_reverse)(ut_printer, (void *)&p, _arr(0xaaabf554, 0xaaaa0556, 0xaaaaaaaa), 0, 65);
	assert(strcmp(buf, "8M5D5M1I8M5I5M2D") == 0, "%s", buf);

	#undef _arr
	free(buf);
}

/* dump_cigar test */
unittest()
{
	uint64_t const len = 16384;
	char *buf = (char *)malloc(len);

	#define _arr(...)		( (uint32_t const []){ 0, 0, __VA_ARGS__, 0, 0 } + 2 )
	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0x55555555), 0, 32);
	assert(strcmp(buf, "16M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0x55555555, 0x55555555), 0, 64);
	assert(strcmp(buf, "32M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0x55555555, 0x55555555, 0x55555555, 0x55555555), 0, 128);
	assert(strcmp(buf, "64M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0x55550000, 0x55555555, 0x55555555, 0x55555555), 16, 112);
	assert(strcmp(buf, "56M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0x55555000, 0x55555555, 0x55555555, 0x55555555), 12, 116);
	assert(strcmp(buf, "58M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0x55), 0, 8);
	assert(strcmp(buf, "4M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0x55555000, 0x55555555, 0x55555555, 0x55), 12, 92);
	assert(strcmp(buf, "46M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0x55550555), 0, 32);
	assert(strcmp(buf, "6M4D8M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0x5555f555), 0, 32);
	assert(strcmp(buf, "6M4I8M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0xaaaa0555), 0, 33);
	assert(strcmp(buf, "6M5D8M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0xaaabf555), 0, 33);
	assert(strcmp(buf, "6M5I8M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0xaaabf555, 0xaaaa0556), 0, 65);
	assert(strcmp(buf, "6M5I8M1I5M5D8M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0xaaabf555, 0xaaaa0556, 0xaaaaaaaa), 0, 65);
	assert(strcmp(buf, "6M5I8M1I5M5D8M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_forward)(buf, len, _arr(0xaaabf554, 0xaaaa0556, 0xaaaaaaaa), 0, 65);
	assert(strcmp(buf, "2D5M5I8M1I5M5D8M") == 0, "%s", buf);

	#undef _arr
	free(buf);
}

unittest()
{
	uint64_t const len = 16384;
	char *buf = (char *)malloc(len);

	#define _arr(...)		( (uint32_t const []){ 0, 0, __VA_ARGS__, 0, 0 } + 2 )
	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0x55555555), 0, 32);
	assert(strcmp(buf, "16M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0x55555555, 0x55555555), 0, 64);
	assert(strcmp(buf, "32M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0x55555555, 0x55555555, 0x55555555, 0x55555555), 0, 128);
	assert(strcmp(buf, "64M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0x55550000, 0x55555555, 0x55555555, 0x55555555), 16, 112);
	assert(strcmp(buf, "56M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0x55555000, 0x55555555, 0x55555555, 0x55555555), 12, 116);
	assert(strcmp(buf, "58M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0x55), 0, 8);
	assert(strcmp(buf, "4M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0x55555000, 0x55555555, 0x55555555, 0x55), 12, 92);
	assert(strcmp(buf, "46M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0x55550555), 0, 32);
	assert(strcmp(buf, "8M4D6M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0x5555f555), 0, 32);
	assert(strcmp(buf, "8M4I6M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0xaaaa0555), 0, 33);
	assert(strcmp(buf, "8M5D6M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0xaaabf555), 0, 33);
	assert(strcmp(buf, "8M5I6M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0xaaabf555, 0xaaaa0556), 0, 65);
	assert(strcmp(buf, "8M5D5M1I8M5I6M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0xaaabf555, 0xaaaa0556, 0xaaaaaaaa), 0, 65);
	assert(strcmp(buf, "8M5D5M1I8M5I6M") == 0, "%s", buf);

	_export(gaba_dp_dump_cigar_reverse)(buf, len, _arr(0xaaabf554, 0xaaaa0556, 0xaaaaaaaa), 0, 65);
	assert(strcmp(buf, "8M5D5M1I8M5I5M2D") == 0, "%s", buf);

	#undef _arr
	free(buf);
}

/* cross tests */
/**
 * @struct unittest_naive_result_s
 * @brief result container
 */
struct unittest_naive_result_s {
	int32_t score;
	uint32_t path_length;
	int64_t apos, bpos;
	int64_t alen, blen;
	char *path;
};

/**
 * @fn unittest_naive
 *
 * @brief naive implementation of the forward semi-global alignment algorithm
 * left-aligned gap and left-aligned deletion
 */
#define UNITTEST_SEQ_MARGIN			( 8 )			/* add margin to avoid warnings in the glibc strlen */
#if MODEL == LINEAR
static
struct unittest_naive_result_s unittest_naive(
	struct gaba_params_s const *sc,
	char const *a,
	char const *b)
{
	/* utils */
	#define _a(p, q, plen)	( (q) * ((plen) + 1) + (p) )
	#define s(p, q)			_a(p, (q), alen)
	#define m(p, q)			( a[(p) - 1] == b[(q) - 1] ? m : x )

	/* load gap penalties */
	v16i8_t scv = _loadu_v16i8(sc->score_matrix);
	int8_t m = _hmax_v16i8(scv);
	int8_t x = -_hmax_v16i8(_sub_v16i8(_zero_v16i8(), scv));
	int8_t g = -(sc->gi + sc->ge);

	/* calc lengths */
	int64_t alen = strlen(a);
	int64_t blen = strlen(b);

	/* calc min */
	int64_t min = INT16_MIN - x - 2 * g;

	/* malloc matrix */
	int16_t *mat = (int16_t *)malloc(
		(alen + 1) * (blen + 1) * sizeof(int16_t));

	/* init */
	struct unittest_naive_maxpos_s {
		int16_t score;
		int64_t apos;
		int64_t bpos;
	};

	struct unittest_naive_maxpos_s max = { 0, 0, 0 };

	mat[s(0, 0)] = 0;
	for(int64_t i = 1; i < alen+1; i++) {
		mat[s(i, 0)] = MAX2(min, i * g);
	}
	for(int64_t j = 1; j < blen+1; j++) {
		mat[s(0, j)] = MAX2(min, j * g);
	}

	for(int64_t j = 1; j < blen+1; j++) {
		for(int64_t i = 1; i < alen+1; i++) {
			int16_t score = mat[s(i, j)] = MAX4(min,
				mat[s(i - 1, j - 1)] + m(i, j),
				mat[s(i - 1, j)] + g,
				mat[s(i, j - 1)] + g);
			if(score > max.score
			|| (score == max.score && (i + j) < (max.apos + max.bpos))) {
				max = (struct unittest_naive_maxpos_s){
					score, i, j
				};
			}
		}
	}
	if(max.score == 0) {
		max = (struct unittest_naive_maxpos_s){ 0, 0, 0 };
	}

	debug("max(%d), apos(%lld), bpos(%lld)", max.score, max.apos, max.bpos);

	struct unittest_naive_result_s result = {
		.score = max.score,
		.apos = max.apos,
		.bpos = max.bpos,
		.path_length = max.apos + max.bpos + 1,
		.path = (char *)malloc(max.apos + max.bpos + UNITTEST_SEQ_MARGIN)
	};
	int64_t path_index = max.apos + max.bpos + 1;
	while(max.apos > 0 || max.bpos > 0) {
		debug("path_index(%llu), apos(%lld), bpos(%lld)", path_index, max.apos, max.bpos);

		/* M > I > D > X */
		if(max.bpos > 0
		&& mat[s(max.apos, max.bpos)] == mat[s(max.apos, max.bpos - 1)] + g) {
			max.bpos--;
			result.path[--path_index] = 'D';
		} else if(max.apos > 0
		&& mat[s(max.apos, max.bpos)] == mat[s(max.apos - 1, max.bpos)] + g) {
			max.apos--;
			result.path[--path_index] = 'R';
		} else {
			result.path[--path_index] = 'R';
			result.path[--path_index] = 'D';
			max.apos--;
			max.bpos--;
		}
	}
	result.alen = result.apos - max.apos;
	result.blen = result.bpos - max.bpos;
	result.apos = max.apos;
	result.bpos = max.bpos;

	result.path_length -= path_index;
	for(uint64_t i = 0; i < result.path_length; i++) {
		result.path[i] = result.path[path_index++];
	}
	result.path[result.path_length] = '\0';
	free(mat);

	#undef _a
	#undef s
	#undef m
	return(result);
}
#else /* MODEL == AFFINE */
static
struct unittest_naive_result_s unittest_naive(
	struct gaba_params_s const *sc,
	char const *a,
	char const *b)
{
	/* utils */
	#define _a(p, q, plen)	( (q) * ((plen) + 1) + (p) )
	#define s(p, q)			_a(p, 3*(q), alen)
	#define e(p, q)			_a(p, 3*(q)+1, alen)
	#define f(p, q)			_a(p, 3*(q)+2, alen)
	#define m(p, q)			( a[(p) - 1] == b[(q) - 1] ? m : x )

	/* load gap penalties */
	v16i8_t scv = _loadu_v16i8(sc->score_matrix);
	int8_t m = _hmax_v16i8(scv);
	int8_t x = -_hmax_v16i8(_sub_v16i8(_zero_v16i8(), scv));
	int8_t gi = -sc->gi;
	int8_t ge = -sc->ge;

	/* calc lengths */
	int64_t alen = strlen(a);
	int64_t blen = strlen(b);

	/* calc min */
	int64_t min = INT16_MIN - x - 2*gi;

	/* malloc matrix */
	int16_t *mat = (int16_t *)malloc(
		3 * (alen + 1) * (blen + 1) * sizeof(int16_t));

	/* init */
	struct unittest_naive_maxpos_s {
		int16_t score;
		int64_t apos;
		int64_t bpos;
	};

	struct unittest_naive_maxpos_s max = { 0, 0, 0 };

	mat[s(0, 0)] = mat[e(0, 0)] = mat[f(0, 0)] = 0;
	for(int64_t i = 1; i < alen+1; i++) {
		mat[s(i, 0)] = mat[e(i, 0)] = MAX2(min, gi + i * ge);
		mat[f(i, 0)] = MAX2(min, gi + i * ge + gi - 1);
	}
	for(int64_t j = 1; j < blen+1; j++) {
		mat[s(0, j)] = mat[f(0, j)] = MAX2(min, gi + j * ge);
		mat[e(0, j)] = MAX2(min, gi + j * ge + gi - 1);
	}

	for(int64_t j = 1; j < blen+1; j++) {
		for(int64_t i = 1; i < alen+1; i++) {
			int16_t score_e = mat[e(i, j)] = MAX2(
				mat[s(i - 1, j)] + gi + ge,
				mat[e(i - 1, j)] + ge);
			int16_t score_f = mat[f(i, j)] = MAX2(
				mat[s(i, j - 1)] + gi + ge,
				mat[f(i, j - 1)] + ge);
			int16_t score = mat[s(i, j)] = MAX4(min,
				mat[s(i - 1, j - 1)] + m(i, j),
				score_e, score_f);
			if(score > max.score
			|| (score == max.score && (i + j) < (max.apos + max.bpos))) {
				max = (struct unittest_naive_maxpos_s){
					score, i, j
				};
			}
		}
	}
	if(max.score == 0) {
		max = (struct unittest_naive_maxpos_s){ 0, 0, 0 };
	}

	struct unittest_naive_result_s result = {
		.score = max.score,
		.apos = max.apos,
		.bpos = max.bpos,
		.path_length = max.apos + max.bpos + 1,
		.path = (char *)malloc(max.apos + max.bpos + UNITTEST_SEQ_MARGIN)
	};
	int64_t path_index = max.apos + max.bpos + 1;
	while(max.apos > 0 || max.bpos > 0) {
		/* M > I > D > X */
		if(mat[s(max.apos, max.bpos)] == mat[f(max.apos, max.bpos)]) {
			while(mat[f(max.apos, max.bpos)] == mat[f(max.apos, max.bpos - 1)] + ge) {
				max.bpos--;
				result.path[--path_index] = 'D';
			}
			max.bpos--;
			result.path[--path_index] = 'D';
		} else if(mat[s(max.apos, max.bpos)] == mat[e(max.apos, max.bpos)]) {
			while(mat[e(max.apos, max.bpos)] == mat[e(max.apos - 1, max.bpos)] + ge) {
				max.apos--;
				result.path[--path_index] = 'R';
			}
			max.apos--;
			result.path[--path_index] = 'R';
		} else {
			result.path[--path_index] = 'R';
			result.path[--path_index] = 'D';
			max.apos--;
			max.bpos--;
		}
	}

	result.alen = result.apos - max.apos;
	result.blen = result.bpos - max.bpos;
	result.apos = max.apos;
	result.bpos = max.bpos;

	result.path_length -= path_index;
	for(uint64_t i = 0; i < result.path_length; i++) {
		result.path[i] = result.path[path_index++];
	}
	result.path[result.path_length] = '\0';
	free(mat);

	#undef _a
	#undef s
	#undef e
	#undef f
	#undef m
	return(result);
}
#endif /* MODEL */

/**
 * @fn unittest_random_base
 */
static
char unittest_random_base(void)
{
	char const table[4] = {'A', 'C', 'G', 'T'};
	return(table[rand() % 4]);
}

/**
 * @fn unittest_generate_random_sequence
 */
static
char *unittest_generate_random_sequence(
	int64_t len)
{
	char *seq;		/** a pointer to sequence */
	seq = (char *)malloc(sizeof(char) * (len + UNITTEST_SEQ_MARGIN));

	if(seq == NULL) { return NULL; }
	for(int64_t i = 0; i < len; i++) {
		seq[i] = unittest_random_base();
	}
	seq[len] = '\0';
	return seq;
}

/**
 * @fn unittest_generate_mutated_sequence
 */
static
char *unittest_generate_mutated_sequence(
	char const *seq,
	double x,
	double d,
	int bw)
{
	if(seq == NULL) { return NULL; }

	int64_t wave = 0;			/** wave is q-coordinate of the alignment path */
	int64_t len = strlen(seq);
	char *mutated_seq = (char *)malloc(sizeof(char) * (len + UNITTEST_SEQ_MARGIN));
	if(mutated_seq == NULL) { return NULL; }
	for(int64_t i = 0, j = 0; i < len; i++) {
		if(((double)rand() / (double)RAND_MAX) < x) {
			mutated_seq[i] = unittest_random_base();	j++;	/** mismatch */
		} else if(((double)rand() / (double)RAND_MAX) < d) {
			if(rand() & 0x01 && wave > -bw+1) {
				mutated_seq[i] = (j < len) ? seq[j++] : unittest_random_base();
				j++; wave--;						/** deletion */
			} else if(wave < bw-2) {
				mutated_seq[i] = unittest_random_base();
				wave++;								/** insertion */
			} else {
				mutated_seq[i] = (j < len) ? seq[j++] : unittest_random_base();
			}
		} else {
			mutated_seq[i] = (j < len) ? seq[j++] : unittest_random_base();
		}
	}
	mutated_seq[len] = '\0';
	return mutated_seq;
}

/**
 * @fn unittest_add_tail
 */
static
char *unittest_add_tail(
	char *seq,
	char c,
	int64_t tail_len)
{
	int64_t len = strlen(seq);
	seq = realloc(seq, len + tail_len + UNITTEST_SEQ_MARGIN);

	for(int64_t i = 0; i < tail_len; i++) {
		seq[len + i] = (c == 0) ? unittest_random_base() : c;
	}
	seq[len + tail_len] = '\0';
	return(seq);
}

/* test if the naive implementation is sane */
#define check_naive_result(_r, _score, _path) ( \
	   (_r).score == (_score) \
	&& strcmp((_r).path, (_path)) == 0 \
	&& (_r).path_length == strlen(_path) \
)
#define print_naive_result(_r) \
	"score(%d), path(%s), len(%d)", \
	(_r).score, (_r).path, (_r).path_length

static
char *string_pair_diff(
	char const *a,
	char const *b)
{
	uint64_t len = 2 * (strlen(a) + strlen(b));
	char *base = malloc(len);
	char *ptr = base, *tail = base + len - 1;
	uint64_t state = 0;

	#define push(ch) { \
		*ptr++ = (ch); \
		if(ptr == tail) { \
			base = realloc(base, 2 * len); \
			ptr = base + len; \
			tail = base + 2 * len; \
			len *= 2; \
		} \
	}
	#define push_str(str) { \
		for(uint64_t i = 0; i < strlen(str); i++) { \
			push(str[i]); \
		} \
	}

	uint64_t i;
	for(i = 0; i < MIN2(strlen(a), strlen(b)); i++) {
		if(state == 0 && a[i] != b[i]) {
			push_str("\x1b[31m"); state = 1;
		} else if(state == 1 && a[i] == b[i]) {
			push_str("\x1b[39m"); state = 0;
		}
		push(a[i]);
	}
	if(state == 1) { push_str("\x1b[39m"); state = 0; }
	for(; i < strlen(a); i++) { push(a[i]); }

	push('\n');
	for(uint64_t i = 0; i < strlen(b); i++) {
		push(b[i]);
	}

	push('\0');
	return(base);
}
#define format_string_pair_diff(_a, _b) ({ \
	char *str = string_pair_diff(_a, _b); \
	char *copy = alloca(strlen(str) + 1); \
	strcpy(copy, str); \
	free(str); \
	copy; \
})
#define print_string_pair_diff(_a, _b)		"\n%s", format_string_pair_diff(_a, _b)

#if MODEL == LINEAR
unittest()
{
	struct gaba_params_s const *p = unittest_default_params;
	struct unittest_naive_result_s n;

	/* all matches */
	n = unittest_naive(p, "AAAA", "AAAA");
	assert(check_naive_result(n, 8, "DRDRDRDR"), print_naive_result(n));
	free(n.path);

	/* with deletions */
	n = unittest_naive(p, "TTTTACGTACGT", "TTACGTACGT");
	assert(check_naive_result(n, 8, "DRDRRRDRDRDRDRDRDRDRDR"), print_naive_result(n));
	free(n.path);

	/* with insertions */
	n = unittest_naive(p, "TTACGTACGT", "TTTTACGTACGT");
	assert(check_naive_result(n, 8, "DRDRDDDRDRDRDRDRDRDRDR"), print_naive_result(n));
	free(n.path);
}
#else /* MODEL == AFFINE */
unittest()
{
	struct gaba_params_s const *p = unittest_default_params;
	struct unittest_naive_result_s n;

	/* all matches */
	n = unittest_naive(p, "AAAA", "AAAA");
	assert(check_naive_result(n, 8, "DRDRDRDR"), print_naive_result(n));
	free(n.path);

	/* with deletions */
	n = unittest_naive(p, "TTTTACGTACGT", "TTACGTACGT");
	assert(check_naive_result(n, 13, "DRDRRRDRDRDRDRDRDRDRDR"), print_naive_result(n));
	free(n.path);

	/* with insertions */
	n = unittest_naive(p, "TTACGTACGT", "TTTTACGTACGT");
	assert(check_naive_result(n, 13, "DRDRDDDRDRDRDRDRDRDRDR"), print_naive_result(n));
	free(n.path);

	/* ins-match-del */
	n = unittest_naive(p, "ATGAAGCTGCGAGGC", "TGATGGCTTGCGAGGC");
	assert(check_naive_result(n, 6, "DDDRDRDRRRDRDRDRDDRDRDRDRDRDRDR"), print_naive_result(n));
	free(n.path);
}
#endif /* MODEL */

#define UNITTEST_MAX_SEQ_CNT		( 16 )
struct unittest_seq_pair_s {
	char const *a[UNITTEST_MAX_SEQ_CNT];
	char const *b[UNITTEST_MAX_SEQ_CNT];
};

struct unittest_sec_pair_s {
	struct gaba_section_s *a;
	struct gaba_section_s *b;
	uint32_t apos, bpos;
};

static
uint8_t unittest_encode_base(char c)
{
	/* convert to upper case and subtract offset by 0x40 */
	#define _b(x)	( (x) & 0x1f )

	/* conversion tables */
	enum bases { A = 0x01, C = 0x02, G = 0x04, T = 0x08 };
	static uint8_t const table[] = {
		[_b('A')] = A,
		[_b('C')] = C,
		[_b('G')] = G,
		[_b('T')] = T,
		[_b('U')] = T,
		[_b('R')] = A | G,
		[_b('Y')] = C | T,
		[_b('S')] = G | C,
		[_b('W')] = A | T,
		[_b('K')] = G | T,
		[_b('M')] = A | C,
		[_b('B')] = C | G | T,
		[_b('D')] = A | G | T,
		[_b('H')] = A | C | T,
		[_b('V')] = A | C | G,
		[_b('N')] = 0,		/* treat 'N' as a gap */
		[_b('_')] = 0		/* sentinel */
	};
	return(table[_b((uint8_t)c)]);

	#undef _b
}

static
char *unittest_cat_seq(char const **p)
{
	uint64_t len = 0;
	for(char const **q = p; *q != NULL; q++) {
		len += strlen(*q);
	}
	char *b = malloc(len + 1), *s = b;
	for(char const **q = p; *q != NULL; q++) {
		memcpy(s, *q, strlen(*q)); s += strlen(*q);
	}
	*s = '\0';
	return(b);
}

static
struct gaba_section_s *unittest_build_section_forward(char const **p)
{
	struct gaba_section_s *s = calloc(UNITTEST_MAX_SEQ_CNT + 1, sizeof(struct gaba_section_s));

	uint64_t len = 256;
	for(char const **q = p; *q != NULL; q++) { len += strlen(*q) + 1; }
	char *b = calloc(1, len), *a = b; a += 128;
	uint64_t i = 0;
	while(p[i] != NULL) {
		s[i] = gaba_build_section(i * 2, a, strlen(p[i]));
		for(char const *r = p[i]; *r != '\0'; r++) {
			*a++ = unittest_encode_base(*r);
		}
		i++; a++;
	}
	s[i] = gaba_build_section(i * 2, a, BW);
	memset(a, N, BW);
	return(s);
}

static
struct gaba_section_s *unittest_build_section_reverse(char const **p)
{
	struct gaba_section_s *s = calloc(UNITTEST_MAX_SEQ_CNT + 1, sizeof(struct gaba_section_s));

	uint64_t len = 256;
	for(char const **q = p; *q != NULL; q++) { len += strlen(*q) + 1; }
	char *b = calloc(1, len), *a = b; a += 128;
	uint64_t i = 0;
	while(p[i] != NULL) {
		s[i] = gaba_build_section(i * 2 + 1, a, strlen(p[i]));
		for(char const *r = p[i] + strlen(p[i]); r > p[i]; r--) {
			*a++ = unittest_encode_base(r[-1]);
		}
		i++; a++;
	}
	s[i] = gaba_build_section(i * 2 + 1, a, BW);
	memset(a, N, BW);
	return(s);
}

static
void unittest_clean_section(struct unittest_sec_pair_s *s)
{
	free((char *)s->a[0].base + s->apos - 128);
	free((char *)s->b[0].base + s->bpos - 128);
	free(s->a); free(s->b);
	free(s);
	return;
}

static
struct unittest_sec_pair_s *unittest_build_section(
	struct unittest_seq_pair_s *p,
	struct gaba_section_s *(*build_section)(char const **))
{
	struct unittest_sec_pair_s *s = malloc(sizeof(struct unittest_sec_pair_s));
	s->a = build_section(p->a); s->apos = rand() % 64; s->a[0].base -= s->apos; s->a[0].len += s->apos;
	s->b = build_section(p->b); s->bpos = rand() % 64; s->b[0].base -= s->bpos; s->b[0].len += s->bpos;
	return(s);
}

static
struct gaba_fill_s const *unittest_dp_extend(
	struct gaba_dp_context_s *dp,
	struct unittest_sec_pair_s *p)
{
	/* fill root */
	struct gaba_section_s const *a = p->a, *b = p->b;
	struct gaba_fill_s const *f = _export(gaba_dp_fill_root)(dp, a, p->apos, b, p->bpos, 0);

	gaba_fill_t const *m = f;
	while((f->stat & GABA_TERM) == 0) {
		if(f->stat & GABA_UPDATE_A) {
			a++;
			debug("update a(%u, %u, %s)", a->id, a->len, a->base);
		}
		if(f->stat & GABA_UPDATE_B) {
			b++;
			debug("update b(%u, %u, %s)", b->id, b->len, b->base);
		}
		if(a->base == NULL || b->base == NULL) { break; }
		f = _export(gaba_dp_fill)(dp, f, a, b, 0);
		m = f->max > m->max ? f : m;
	}
	return(m);									/* never be null */
}

unittest()
{
	struct unittest_seq_pair_s pairs[] = {
		{
			.a = { "" },
			.b = { "" }
		},
		{
			.a = { "A" },
			.b = { "A" }
		},
		{
			.a = { "A", "A" },
			.b = { "A", "A" }
		},
		{
			.a = { "AA" },
			.b = { "AA" }
		},
		{
			.a = { "ACGT" },
			.b = { "ACGT" }
		},
		{
			.a = { "ACGT", "ACGT" },
			.b = { "ACGT", "ACGT" }
		},
		{
			.a = { "ACGTACGT" },
			.b = { "ACGT", "ACGT" }
		},
		{
			.a = { "ACGT", "ACGT" },
			.b = { "ACGTACGT" }
		},
		{
			.a = { "GAAAAAAAA" },
			.b = { "AAAAAAAA" }
		},
		{
			.a = { "TTTTTTTT" },
			.b = { "CTTTTTTTT" }
		},
		{
			.a = { "GACGTACGT" },
			.b = { "ACGTACGT" }
		},
		{
			.a = { "ACGTACGT" },
			.b = { "GACGTACGT" }
		},
		{
			.a = { "GACGTACGT", "ACGTACGT", "ACGTACGT", "GACGTACGT" },
			.b = { "ACGTACGT", "GACGTACGT", "GACGTACGT", "ACGTACGT" }
		},
		{
			.a = { "GACGTACGTGACGTACGT" },
			.b = { "ACGTACGT", "ACGTACGT" }
		},
		{
			.a = { "ACGTACGT", "ACGTACGT" },
			.b = { "GACGTACGTGACGTACGT" }
		},
		{
			.a = { "ACGTACGT", "ACGTACGT", "ACGTACGT", "ACGTACGT" },
			.b = { "ACGTACGT", "ACGTACGTGG", "TTGACGTACGT", "ACGTACGT" }
		},
	};

	uint8_t const *lim = (uint8_t const *)0x800000000000;
	struct gaba_context_s const *c = (struct gaba_context_s const *)gctx;
	struct gaba_dp_context_s *dp = _export(gaba_dp_init)(c, lim, lim);

	for(uint64_t i = 0; i < sizeof(pairs) / sizeof(struct unittest_seq_pair_s); i++) {

		/* test naive implementations */
		char *a = unittest_cat_seq(pairs[i].a);
		char *b = unittest_cat_seq(pairs[i].b);
		struct unittest_naive_result_s nr = unittest_naive(unittest_default_params, a, b);

		assert(nr.score >= 0);
		assert(nr.path_length >= 0);
		assert(nr.path != NULL);

		/* fill-in sections */
		struct unittest_sec_pair_s *s = unittest_build_section(
			&pairs[i],
			unittest_build_section_forward
		);
		struct gaba_fill_s const *m = unittest_dp_extend(dp, s);

		assert(m != NULL);
		assert(m->max == nr.score, "m->max(%lld), nr.score(%d)", m->max, nr.score);

		/* test path */
		struct gaba_alignment_s const *r = _export(gaba_dp_trace)(dp, m, NULL);

		assert(r != NULL);
		assert(r->plen == nr.path_length, "m->plen(%llu), nr.path_length(%u)", r->plen, nr.path_length);
		assert(check_path(r, nr.path), "r->path(%s), nr.path(%s)", decode_path(r), nr.path);

		unittest_clean_section(s);
	}

	_export(gaba_dp_clean)(dp);
}

#if 0
#if 1
/* cross test */
unittest()
{
	uint8_t const *lim = (uint8_t const *)0x800000000000;
	struct gaba_context_s const *c = (struct gaba_context_s const *)gctx;
	struct gaba_params_s const *p = unittest_default_params;

	/* seed rand */
	#ifndef SEED
	int32_t seed = getpid();
	#else
	int32_t seed = SEED;
	#endif
	srand(seed);

	// int64_t cross_test_count = 10000000;
	int64_t cross_test_count = 1000;
	for(int64_t i = 0; i < cross_test_count; i++) {
		/* generate sequences */
		char *a = unittest_generate_random_sequence(1000);
		char *b = unittest_generate_mutated_sequence(a, 0.1, 0.1, 500);

		/* add random sequences at the tail */
		a = unittest_add_tail(a, 0, 64);
		b = unittest_add_tail(b, 0, 64);

		/* add tail margin */
		int64_t const mlen = 20;
		a = unittest_add_tail(a, 'C', mlen);
		b = unittest_add_tail(b, 'G', mlen);


		/* naive */
		struct unittest_naive_result_s nf = unittest_naive(p, a, b);

		/* build section */
		struct unittest_sections_s *sec = unittest_build_seqs(
			&((struct unittest_seqs_s){ .a = a, .b = b }));

		debug("seed(%d)\n%s", seed, format_string_pair_diff(a, b));

		/* generate dp context */
		struct gaba_dp_context_s *d = _export(gaba_dp_init)(c, lim, lim);

		/* fill section */
		struct gaba_section_s const *as = &sec->afsec;
		struct gaba_section_s const *bs = &sec->bfsec;
		struct gaba_fill_s *f = _export(gaba_dp_fill_root)(d, as, 0, bs, 0);
		struct gaba_fill_s *m = f;

		/* fill tail (1) */
		as = (f->stat & GABA_UPDATE_A) ? &sec->aftail : as;
		bs = (f->stat & GABA_UPDATE_B) ? &sec->bftail : bs;
		struct gaba_fill_s *t1 = _export(gaba_dp_fill)(d, f, as, bs);
		m = (t1->max > m->max) ? t1 : m;

		/* fill tail (2) */
		as = (t1->stat & GABA_UPDATE_A) ? &sec->aftail : as;
		bs = (t1->stat & GABA_UPDATE_B) ? &sec->bftail : bs;
		struct gaba_fill_s *t2 = _export(gaba_dp_fill)(d, t1, as, bs);
		m = (t2->max > m->max) ? t2 : m;

		/* check max */
		assert(m->max == nf.score, "m->max(%lld), f(%lld, %u), t1->max(%lld, %u), t2->max(%lld, %u), n.score(%d)",
			m->max, f->max, f->stat, t1->max, t1->stat, t2->max, t2->stat, nf.score);
		if(m->max != nf.score) {
			struct gaba_fill_s *f2 = _export(gaba_dp_fill_root)(d, &sec->afsec, 0, &sec->bfsec, 0);
			(void)f2;
			debug("refill f2(%lld, %u)", f2->max, f2->stat);
		}

		/* forward trace */
		struct gaba_alignment_s *rf = _export(gaba_dp_trace)(d, m, NULL);

		/* check results */
		assert(rf->score == nf.score, "m->max(%lld), rf->score(%lld), nf.score(%d)",
			m->max, rf->score, nf.score);
		assert(rf->seg[0].apos == nf.apos, "apos(%u, %lld)", rf->seg[0].apos, nf.apos);
		assert(rf->seg[0].bpos == nf.bpos, "bpos(%u, %lld)", rf->seg[0].bpos, nf.bpos);
		assert(rf->seg[0].alen == nf.alen, "alen(%u, %lld)", rf->seg[0].alen, nf.alen);
		assert(rf->seg[0].blen == nf.blen, "blen(%u, %lld)", rf->seg[0].blen, nf.blen);
		assert(check_path(rf, nf.path), "\n%s\n%s\n%s",
			a, b, format_string_pair_diff(decode_path(rf), nf.path));

		int64_t acnt = 0, bcnt = 0;
		for(int64_t i = 0; i < rf->plen; i++) {
			if(((rf->path[i / 32]>>(i & 31)) & 0x01) == 0) {
				acnt++;
			} else {
				bcnt++;
			}
		}
		assert(acnt == rf->seg[0].alen, "acnt(%lld), alen(%u)", acnt, rf->seg[0].alen);
		assert(bcnt == rf->seg[0].blen, "bcnt(%lld), blen(%u)", bcnt, rf->seg[0].blen);

		debug("score(%lld, %d), alen(%lld), blen(%lld)\n%s",
			rf->score, nf.score, nf.alen, nf.blen,
			format_string_pair_diff(decode_path(rf), nf.path));

		/* cleanup */
		_export(gaba_dp_clean)(d);
		free(sec);
		free(nf.path);
		free(a);
		free(b);
	}
}
#else
/* for debugging */
unittest(with_seq_pair(
"CTGCGCGAGTCTGCCATGAAATCGAGCTTACAATCCCGATCTTCTCAGCCCTATTGCGGATAGTAGTATATTCA",
"ACGTGCGCGGTGGTTGCTCTTCTGGACGCGTTCGACACGTATTACGAAGTCCTTACCGCTATAAATCACAACGC"))
{
	omajinai();
	struct gaba_params_s const *p = unittest_default_params;

	/* fill section */
	struct gaba_section_s const *as = &s->afsec;
	struct gaba_section_s const *bs = &s->bfsec;
	struct gaba_fill_s *f = _export(gaba_dp_fill_root)(d, as, 0, bs, 0);

	/* fill tail (1) */
	as = (f->stat & GABA_UPDATE_A) ? &s->aftail : as;
	bs = (f->stat & GABA_UPDATE_B) ? &s->bftail : bs;
	struct gaba_fill_s *t1 = _export(gaba_dp_fill)(d, f, as, bs);
	f = (t1->max > f->max) ? t1 : f;

	/* fill tail (2) */
	as = (f->stat & GABA_UPDATE_A) ? &s->aftail : as;
	bs = (f->stat & GABA_UPDATE_B) ? &s->bftail : bs;
	struct gaba_fill_s *t2 = _export(gaba_dp_fill)(d, t1, as, bs);
	f = (t2->max > f->max) ? t2 : f;
	struct gaba_result_s *r = _export(gaba_dp_trace)(d, f, NULL, NULL);

	/* naive */
	char const *a = (char const *)s->a;
	char const *b = (char const *)s->b;
	struct unittest_naive_result_s n = unittest_naive(p, a, b);

	/* check scores */
	assert(r->score == n.score, "f->max(%lld), r->score(%lld), n.score(%d)",
		f->max, r->score, n.score);
	assert(check_path(r, n.path), "\n%s\n%s\n%s",
		a, b, format_string_pair_diff(decode_path(r), n.path));

	debug("score(%lld, %d), alen(%lld), blen(%lld)\n%s",
		r->score, n.score, n.alen, n.blen,
		format_string_pair_diff(decode_path(r), n.path));

	/* cleanup */
	_export(gaba_dp_clean)(d);
}
#endif
#endif

#endif /* UNITTEST */

/**
 * end of gaba.c
 */
