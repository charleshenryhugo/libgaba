
/*
 * @file util.h
 *
 * @brief a collection of utilities
 */

#ifndef _UTIL_H_INCLUDED
#define _UTIL_H_INCLUDED

#include "../sea.h"
#include "log.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/**
 * structs
 */

/**
 * @struct sea_pos
 *
 * @brief (internal) holds a pair of (x, y)-coordinate and (p, q)-coordinate.
 */
struct sea_pos {
	int64_t i, j, p, q;
};

/**
 * @struct sea_mem
 *
 * @brief (internal) sequence reader
 */
struct sea_mem {
	uint8_t *sp, *ep;
};

/**
 * @struct sea_reader
 *
 * @brief (internal) abstract sequence reader
 */
struct sea_reader {
	uint8_t *p;
	uint8_t (*pop)(
		uint8_t const *p,
		int64_t pos);
	uint8_t b;
};

/**
 * @struct sea_writer
 *
 * @brief (internal) abstract sequence writer
 */
struct sea_writer {
	uint8_t *p;
	int64_t pos, size, len;
	int64_t (*init)(
		uint8_t *ptr,
		int64_t fpos,
		int64_t rpos);
	int64_t (*pushm)(
		uint8_t *p,
		int64_t pos);
	int64_t (*pushx)(
		uint8_t *p,
		int64_t pos);
	int64_t (*pushi)(
		uint8_t *p,
		int64_t pos);
	int64_t (*pushd)(
		uint8_t *p,
		int64_t pos);
	int64_t (*finish)(
		uint8_t *ptr,
		int64_t pos);
};

/**
 * @struct sea_ivec
 *
 * @brief (internal) init vector container. (sizeof(struct sea_ivec) == 64)
 */
struct sea_ivec {
	int64_t i, j, p, q;			/*!< (32) terminal coordinate */
	int32_t *pv, *cv;			/*!< (16) pointers to the initial vectors (should be allocated in the stack by fixed length array, or alloca) */
//	uint8_t dr;					/*!< previous direction */
	uint32_t len;				/*!< (4) length of the pv vector */
//	uint8_t *prev;				/*!< pointer to the previous block (previous pdp) */
//	uint8_t _pad[4];			/*!< (4) */
	int32_t max;				/*!< (4) max of the next block */
	int64_t ep;					/*!< (8) terminal p-coordinate of the next band */
};

#define _ivec(pdp, member) 		((struct sea_ivec *)(pdp) - 1)->member
#define DEF_VEC_LEN				( 32 )		/** default vector length */

/**
 * @struct sea_coords
 * @brief (internal) a set of local-nonvolatile variables. (need write back / caller saved values)
 */
struct sea_coords {
	int64_t i, j, p, q;			/*!< temporary */
	struct sea_writer l;		/*!< (inout) alignment writer */
};

/**
 * @struct sea_process
 *
 * @brief (internal) a set of local-volatile variables. (no need to write back)
 */
struct sea_process {
	struct sea_ivec v;
	struct sea_reader a, b;		/*!< (in) sequence readers */
	struct sea_mem dp, dr;		/*!< (ref) a dynamic programming matrix */
};

/**
 * @struct sea_local_context
 *
 * @brief (internal) local constant container.
 */
struct sea_local_context {
	struct sea_aln_funcs const *f;

	int8_t m;					/*!< a dynamic programming cost: match */
	int8_t x;					/*!< a dynamic programming cost: mismatch */
	int8_t gi;					/*!< a dynamic programming cost: gap open (in the affine-gap cost) or gap cost per base (in the linear-gap cost) */
	int8_t ge;					/*!< a dynamic programming cost: gap extension */

	int8_t k;					/*!< heuristic search length stretch ratio. (default is k = 4) */
	int8_t bw;					/*!< the width of the band. */
	int16_t tx;					/*!< xdrop threshold. see sea_init for more details */
	int32_t min;				/*!< (in) lower bound of the score */
	uint32_t alg;				/*!< algorithm flag (same as (ctx->flags) & SEA_FLAG_MASK_ALG) */

	size_t isize;				/*!< initial matsize */
//	size_t memaln;				/*!< memory alignment size (default: 32) */

	uint8_t *pdp;				/*!< dynamic programming matrix */
	uint8_t *tdp;				/*!< the end of dp matrix */
	uint8_t *pdr;				/*!< direction array */
	uint8_t *tdr;				/*!< the end of direction array */
	int64_t asp, bsp;			/*!< the start position on the sequences */
	int64_t aep, bep;			/*!< the end position on the sequences */
//	int64_t alim, blim;			/*!< the limit coordinate of the band */
//	int64_t size;				/*!< default malloc size */

	int8_t do_trace;			/*!< do traceback if nonzero */
	int8_t _pad[3];

	int32_t max;				/*!< (inout) current maximum score */
	int64_t mi, mj, mp, mq;		/*!< maximum score position */

	struct sea_reader a, b;		/*!< (in) sequence readers */
	struct sea_writer l;		/*!< (inout) alignment writer */
};

/**
 * @struct sea_aln_funcs
 *
 * @brief (internal) a struct which holds alignment function pointers.
 */
struct sea_aln_funcs {
	int32_t (*twig)(			/*!< diag 8bit */
		struct sea_local_context *this);
	int32_t (*branch)(			/*!< diag 16bit */
		struct sea_local_context *this);
	int32_t (*trunk)(			/*!< diag 32bit */
		struct sea_local_context *this);
	int32_t (*balloon)(			/*!< 32bit balloon algorithm */
		struct sea_local_context *this);
	int32_t (*bulge)(			/*!< 32bit bulge (guided balloon) algorithm */
		struct sea_local_context *this);
	int32_t (*cap)(				/*!< 32bit cap algorithm */
		struct sea_local_context *this);
};

/**
 * @struct sea_io_funcs
 * @brief (internal) a struct which holds io function pointers.
 */
struct sea_io_funcs {
	uint8_t (*popa)(			/*!< retrieve a character from sequence a */
		uint8_t const *ptr,
		int64_t pos);
	uint8_t (*popb)(
		uint8_t const *ptr,
		int64_t pos);
	int64_t (*init)(			/*!< forward alnwriter */
		uint8_t *ptr,
		int64_t fpos,
		int64_t rpos);
	int64_t (*pushm)(
		uint8_t *ptr,
		int64_t pos);
	int64_t (*pushx)(
		uint8_t *ptr,
		int64_t pos);
	int64_t (*pushi)(
		uint8_t *ptr,
		int64_t pos);
	int64_t (*pushd)(
		uint8_t *ptr,
		int64_t pos);
	int64_t (*finish)(
		uint8_t *ptr,
		int64_t pos);
};

/**
 * @struct sea_context
 *
 * @brief (API) an algorithmic context.
 *
 * @sa sea_init, sea_close
 */
struct sea_context {
	struct sea_aln_funcs dynamic, guided;
	struct sea_io_funcs fw, rv;
	struct sea_ivec v;
	int flags;			/*!< a bitfield of option flags */
};

/**
 * function declarations
 * naive, twig, branch, trunk, balloon, bulge, cap
 */
int32_t naive_linear_dynamic(struct sea_local_context *this);
int32_t naive_affine_dynamic(struct sea_local_context *this);
int32_t naive_linear_guided(struct sea_local_context *this);
int32_t naive_affine_guided(struct sea_local_context *this);

int32_t twig_linear_dynamic(struct sea_local_context *this);
int32_t twig_affine_dynamic(struct sea_local_context *this);
int32_t twig_linear_guided(struct sea_local_context *this);
int32_t twig_affine_guided(struct sea_local_context *this);

int32_t branch_linear_dynamic(struct sea_local_context *this);
int32_t branch_affine_dynamic(struct sea_local_context *this);
int32_t branch_linear_guided(struct sea_local_context *this);
int32_t branch_affine_guided(struct sea_local_context *this);

int32_t trunk_linear_dynamic(struct sea_local_context *this);
int32_t trunk_affine_dynamic(struct sea_local_context *this);
int32_t trunk_linear_guided(struct sea_local_context *this);
int32_t trunk_affine_guided(struct sea_local_context *this);

int32_t balloon_linear_dynamic(struct sea_local_context *this);
int32_t balloon_affine_dynamic(struct sea_local_context *this);
int32_t balloon_linear_guided(struct sea_local_context *this);
int32_t balloon_affine_guided(struct sea_local_context *this);

int32_t bulge_linear_dynamic(struct sea_local_context *this);
int32_t bulge_affine_dynamic(struct sea_local_context *this);
int32_t bulge_linear_guided(struct sea_local_context *this);
int32_t bulge_affine_guided(struct sea_local_context *this);

int32_t cap_linear_dynamic(struct sea_local_context *this);
int32_t cap_affine_dynamic(struct sea_local_context *this);
int32_t cap_linear_guided(struct sea_local_context *this);
int32_t cap_affine_guided(struct sea_local_context *this);

/**
 * io functions
 */

/**
 * @fn _pop_ascii
 * @brief retrieve an ascii character from ((uint8_t *)p)[pos].
 * @detail implemented in `io.s'.
 */
uint8_t _pop_ascii(uint8_t const *p, int64_t pos);

/**
 * @fn _pop_4bit
 * @brief retrieve a 4-bit encoded base from ((uint8_t *)p)[pos].
 * @detail implemented in `io.s'.
 */
uint8_t _pop_4bit(uint8_t const *p, int64_t pos);

/**
 * @fn _pop_2bit
 * @brief retrieve a 2-bit encoded base from ((uint8_t *)p)[pos].
 * @detail implemented in `io.s'.
 */
uint8_t _pop_2bit(uint8_t const *p, int64_t pos);

/**
 * @fn _pop_4bit8packed
 * @brief retrieve a packed 4-bit encoded base from ((uint8_t *)p)[pos/2].
 * @detail implemented in `io.s'.
 */
uint8_t _pop_4bit8packed(uint8_t const *p, int64_t pos);

/**
 * @fn _pop_2bit8packed
 * @brief retrieve a packed 2-bit encoded base from ((uint8_t *)p)[pos/4].
 * @detail implemented in `io.s'.
 */
uint8_t _pop_2bit8packed(uint8_t const *p, int64_t pos);

/**
 * @fn _pushm_ascii_f, pushx_ascii_f, _pushi_ascii_f, _pushd_ascii_f
 * @brief push a match (mismatch, ins, del) character to p[pos]
 * @detail implemented in `io.s'.
 * @return the next pos.
 */
/** forward writer */
int64_t _init_ascii_f(uint8_t *p, int64_t fpos, int64_t rpos);
int64_t _pushm_ascii_f(uint8_t *p, int64_t pos);
int64_t _pushx_ascii_f(uint8_t *p, int64_t pos);
int64_t _pushi_ascii_f(uint8_t *p, int64_t pos);
int64_t _pushd_ascii_f(uint8_t *p, int64_t pos);
int64_t _finish_ascii_f(uint8_t *p, int64_t pos);

/** reverse writer */
int64_t _init_ascii_r(uint8_t *p, int64_t fpos, int64_t rpos);
int64_t _pushm_ascii_r(uint8_t *p, int64_t pos);
int64_t _pushx_ascii_r(uint8_t *p, int64_t pos);
int64_t _pushi_ascii_r(uint8_t *p, int64_t pos);
int64_t _pushd_ascii_r(uint8_t *p, int64_t pos);
int64_t _finish_ascii_r(uint8_t *p, int64_t pos);

/**
 * @fn _pushm_cigar_f, _pushx_cigar_f, _pushi_cigar_f, _pushd_cigar_f
 * @brief append a match (mismatch, ins, del) to p[pos].
 * @detail implemented in `io.s'.
 * @return the next pos.
 */
/** forward writer */
int64_t _init_cigar_f(uint8_t *p, int64_t fpos, int64_t rpos);
int64_t _pushm_cigar_f(uint8_t *p, int64_t pos);
int64_t _pushx_cigar_f(uint8_t *p, int64_t pos);
int64_t _pushi_cigar_f(uint8_t *p, int64_t pos);
int64_t _pushd_cigar_f(uint8_t *p, int64_t pos);
int64_t _finish_cigar_f(uint8_t *p, int64_t pos);

/** reverse writer */
int64_t _init_cigar_r(uint8_t *p, int64_t fpos, int64_t rpos);
int64_t _pushm_cigar_r(uint8_t *p, int64_t pos);
int64_t _pushx_cigar_r(uint8_t *p, int64_t pos);
int64_t _pushi_cigar_r(uint8_t *p, int64_t pos);
int64_t _pushd_cigar_r(uint8_t *p, int64_t pos);
int64_t _finish_cigar_r(uint8_t *p, int64_t pos);

/**
 * @fn _pushm_dir_f, _pushx_dir_f, _pushi_dir_f, _pushd_dir_f
 * @brief append a direction string to p[pos].
 * @detail implemented in `io.s'.
 * @return the next pos.
 */
/** forward writer */
int64_t _init_dir_f(uint8_t *p, int64_t fpos, int64_t rpos);
int64_t _pushm_dir_f(uint8_t *p, int64_t pos);
int64_t _pushx_dir_f(uint8_t *p, int64_t pos);
int64_t _pushi_dir_f(uint8_t *p, int64_t pos);
int64_t _pushd_dir_f(uint8_t *p, int64_t pos);
int64_t _finish_dir_f(uint8_t *p, int64_t pos);

/** reverse writer */
int64_t _init_dir_r(uint8_t *p, int64_t fpos, int64_t rpos);
int64_t _pushm_dir_r(uint8_t *p, int64_t pos);
int64_t _pushx_dir_r(uint8_t *p, int64_t pos);
int64_t _pushi_dir_r(uint8_t *p, int64_t pos);
int64_t _pushd_dir_r(uint8_t *p, int64_t pos);
int64_t _finish_dir_r(uint8_t *p, int64_t pos);

/**
 * flags
 */
#define SW 								( SEA_SW )
#define NW 								( SEA_NW )
#define SEA 							( SEA_SEA )
#define XSEA 							( SEA_XSEA )

/**
 * coordinate conversion macros
 */
//#define	ADDR(p, q, band)			( (band)*(p)+(q)+(band)/2 )
//#define ADDRI(x, y, band) 			( ADDR(COP(x, y, band), COQ(x, y, band), band) )
#define cox(p, q, band)				( ((p)>>1) - (q) )
#define coy(p, q, band)				( (((p)+1)>>1) + (q) )
#define cop(x, y, band)				( (x) + (y) )
#define coq(x, y, band) 			( ((y)-(x))>>1 )
#define INSIDE(x, y, p, q, band)	( (COX(p, q, band) < (x)) && (COY(p, q, band) < (y)) )

/**
 * @enum _STATE
 */
enum _STATE {
	CONT 	= 0,
	MEM 	= 1,
	CHAIN 	= 2,
	ALT 	= 3,
	CAP 	= 4,
	TERM 	= 5,
	SEARCH  = 6
};


/**
 * sequence reader implementations
 */

/**
 * @macro rd_init
 * @brief initialize a sequence reader instance.
 */
#define rd_init(r, fp, base) { \
	(r).p = (uint8_t *)base; \
	(r).b = 0; \
	(r).pop = fp; \
}

/**
 * @macro rd_fetch
 * @brief fetch a decoded base into r.b.
 */
#define rd_fetch(r, pos) { \
	(r).b = (r).pop((r).p, pos); \
}

/**
 * @macro rd_cmp
 * @brief compare two fetched bases.
 * @return true if two bases are the same, false otherwise.
 */
#define rd_cmp(r1, r2)	( (r1).b == (r2).b )

/**
 * @macro rd_decode
 * @brief get a cached char
 */
#define rd_decode(r)	( (r).b )
/**
 * @macro rd_close
 * @brief fill the instance with zero.
 */
#define rd_close(r) { \
	(r).p = NULL; \
	(r).spos = 0; \
	(r).b = 0; \
	(r).pop = NULL; \
}

/**
 * alignment writer implementations
 */

/**
 * @macro SEA_CLIP_LEN
 * @brief length of the reserved buffer for clip sequence
 */
#define SEA_CLIP_LEN 		( 8 )

/**
 * @enum _ALN_CHAR
 * @brief alignment character, used in ascii output format.
 */
enum _ALN_CHAR {
	MCHAR = 'M',
	XCHAR = 'X',
	ICHAR = 'I',
	DCHAR = 'D'
};

/**
 * @enum _ALN_DIR
 * @brief the direction of alignment string.
 */
enum _ALN_DIR {
	ALN_FW = 0,
	ALN_RV = 1
};

/**
 * @macro wr_init
 * @brief initialize an alignment writer instance.
 */
#define wr_init(w, f) { \
	(w).p = NULL; \
	(w).init = (f).init; \
	(w).pushm = (f).pushm; \
	(w).pushx = (f).pushx; \
	(w).pushi = (f).pushi; \
	(w).pushd = (f).pushd; \
	(w).finish = (f).finish; \
}

/**
 * @macro wr_alloc
 * @brief allocate a memory for the alignment string.
 */
#define wr_alloc_size(s) ( \
	  sizeof(struct sea_result) \
	+ sizeof(uint8_t) * ( \
	    (s) \
	  + 16 - SEA_CLIP_LEN /* margin for xmm bulk write, sizeof(__m128i) == 16. (see pushm_cigar_r in io.s) */ \
	  + SEA_CLIP_LEN /* margin for clip seqs at the head */ \
	  + SEA_CLIP_LEN /* margin for clip seqs at the tail */ ) \
)
#define wr_alloc(w, s) { \
	debug("wr_alloc called"); \
	if((w).p == NULL || (w).size < wr_alloc_size(s)) { \
		if((w).p == NULL) { \
			free((w).p); (w).p = NULL; \
		} \
		(w).size = wr_alloc_size(s); \
		(w).p = malloc((w).size); \
	} \
	(w).pos = (w).init((w).p, \
		(w).size - SEA_CLIP_LEN, /** margin: must be consistent to wr_finish */ \
		sizeof(struct sea_result) + SEA_CLIP_LEN); /** margin: must be consistent to wr_finish */ \
	(w).len = 0; \
}

/**
 * @macro wr_pushm, wr_pushx, wr_pushi, wr_pushd
 * @brief push an alignment character
 */
#define wr_pushm(w) { \
	(w).pos = (w).pushm((w).p, (w).pos); (w).len++; \
	debug("pushm: %c, pos(%lld)", (w).p[(w).pos], (w).pos); \
}
#define wr_pushx(w) { \
	(w).pos = (w).pushx((w).p, (w).pos); (w).len++; \
	debug("pushx: %c, pos(%lld)", (w).p[(w).pos], (w).pos); \
}
#define wr_pushi(w) { \
	(w).pos = (w).pushi((w).p, (w).pos); (w).len++; \
	debug("pushi: %c, pos(%lld)", (w).p[(w).pos], (w).pos); \
}
#define wr_pushd(w) { \
	(w).pos = (w).pushd((w).p, (w).pos); (w).len++; \
	debug("pushd: %c, pos(%lld)", (w).p[(w).pos], (w).pos); \
}

/**
 * @macro wr_finish
 * @brief finish the instance
 * @detail the size of the array (s) must have the same value as given to wr_alloc.
 */
#define wr_finish(w, s) { \
	int64_t p = (w).finish((w).p, (w).pos); \
	if(p <= (w).pos) { \
		(w).size = ((w).size - SEA_CLIP_LEN) - p - 1; \
		(w).pos = p; \
	} else { \
		(w).size = p - (sizeof(struct sea_result) + SEA_CLIP_LEN) - 1; \
		(w).pos = sizeof(struct sea_result) + SEA_CLIP_LEN; \
	} \
}

/**
 * @macro wr_clean
 * @brief free malloc'd memory and fill the instance with zero.
 */
#define wr_clean(w) { \
	if((w).p != NULL) { \
		free((w).p); (w).p = NULL; \
	} \
	(w).size = 0; \
	(w).pos = 0; \
	(w).len = 0; \
	(w).pushm = (w).pushx = (w).pushi = (w).pushd = NULL; \
}

/**
 * direction determiner constants
 */
/**
 * @enum _DIR
 * @brief constants for direction flags.
 */
enum _DIR {
	LEFT 	= SEA_UE_LEFT<<2,
	TOP 	= SEA_UE_TOP<<2
};

/**
 * @enum _DIR2
 */
enum _DIR2 {
	LL = (SEA_UE_LEFT<<0) | (SEA_UE_LEFT<<2),
	LT = (SEA_UE_LEFT<<0) | (SEA_UE_TOP<<2),
	TL = (SEA_UE_TOP<<0) | (SEA_UE_LEFT<<2),
	TT = (SEA_UE_TOP<<0) | (SEA_UE_TOP<<2)
};

/**
 * char vector shift operations
 */
#define pushq(x, y) { \
	vec_char_shift_l(y, y); \
	vec_char_insert_lsb(y, x); \
}

#define pusht(x, y) { \
	vec_char_shift_r(y, y); \
	vec_char_insert_msb(y, x); \
}

/**
 * string concatenation macros
 */

/**
 * @macro JOIN2
 * @brief a macro which takes two string and concatenates them.
 */
#define JOIN2(i,j)						i##j

/**
 * @macro JOIN3
 * @brief a macro which takes three string and concatenates them.
 */
#define JOIN3(i,j,k)					i##j##k

/**
 * @macro JOIN4
 * @brief a macro which takes four string and concatenates them.
 */
#define JOIN4(i,j,k,l)					i##j##k##l

/**
 * @macro Q
 * @brief string quotation
 */
#define Q(a)							#a

/**
 * @macro QUOTE
 * @brief an wrapper of Q
 */
#define QUOTE(a)						Q(a)

/**
 * function name composition macros.
 */


/**
 * @macro HEADER_WITH_SUFFIX
 * @brief an wrapper macro of JOIN2
 */
#define HEADER_WITH_SUFFIX(a,b)			JOIN2(a,b)

/**
 * @macro FUNC_WITH_SUFFIX
 * @brief an wrapper macro of JOIN2, for the use of function name composition.
 */
#define FUNC_WITH_SUFFIX(a,b)			JOIN2(a,b)

/**
 * @macro LABEL_WITH_SUFFIX
 * @breif an wrapper of JOIN3
 */
#define LABEL_WITH_SUFFIX(a,b,c)		JOIN3(a,b,c)

/**
 * @macro DECLARE_FUNC
 * @brief a function declaration macro for static (local in a file) functions.
 */
#define DECLARE_FUNC(file, opt, suffix) static LABEL_WITH_SUFFIX(file, opt, suffix)

/**
 * @macro CALL_FUNC
 * @brief call a local function
 */
#define CALL_FUNC(file, opt, suffix)	LABEL_WITH_SUFFIX(file, opt, suffix)

/**
 * @macro DECLARE_FUNC_GLOBAL
 * @brief a function declaration macro for global functions.
 */
#define DECLARE_FUNC_GLOBAL(file, opt)	FUNC_WITH_SUFFIX(file, opt)

/**
 * @macro CALL_FUNC_GLOBAL
 * @brief a function call macro, which is an wrap of a function name composition macro.
 */
#define CALL_FUNC_GLOBAL(file, opt)		FUNC_WITH_SUFFIX(file, opt)

/**
 * @macro func_next
 */
#define func_next(k, ptr) ( \
	(k->f->twig == ptr) ? k->f->branch : k->f->trunk \
)

/**
 * @macro func_alt
 */
#define func_alt(k, ptr) ( \
	(k->f->balloon == ptr) ? k->f->trunk : k->f->balloon \
)

/**
 * @macro load_coord
 * @brief save current (i, j)-coordinate to (mi, mj)
 */
#define load_coord(k, pdp) { \
	k->mi = _ivec(pdp, i); \
	k->mj = _ivec(pdp, j); \
	k->mp = _ivec(pdp, p); \
	k->mq = _ivec(pdp, q); \
}

/**
 * @macro store_coord
 * @brief load current (mi, mj)-coordinate to (i, j)
 */
#define store_coord(k, pdp) { \
	_ivec(pdp, i) = k->mi; \
	_ivec(pdp, j) = k->mj; \
	_ivec(pdp, p) = k->mp; \
	_ivec(pdp, q) = k->mq; \
}

/**
 * @macro LABEL
 * @brief a label declaration macro.
 */
#define LABEL(file, opt, label) 		LABEL_WITH_SUFFIX(file, opt, label)

/* foreach */
#define _for(iter, cnt)		for(iter = 0; iter < cnt; iter++)

/* boolean */
#define TRUE 			( 1 )
#define FALSE 			( 0 )

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
 * @macro XCUT
 * @brief an macro for xdrop termination.
 */
#define XCUT(d,m,x) 	( ((d) + (x) > (m) ? (d) : SEA_CELL_MIN )

/**
 * @fn _read
 */
int32_t static inline
_read_impl(uint8_t *ptr, int64_t pos, size_t size)
{
	switch(size) {
		case 1: return((int32_t)(((int8_t *)ptr)[pos]));
		case 2: return((int32_t)(((int16_t *)ptr)[pos]));
		case 4: return((int32_t)(((int32_t *)ptr)[pos]));
		case 8: return((int32_t)(((int64_t *)ptr)[pos]));
		default: return 0;
	}
}

int32_t static inline
_read(uint8_t *ptr, int64_t pos, size_t size)
{
	int32_t r = _read_impl(ptr, pos, size);
	debug("_read: r(%d) at %p, %lld, %d", r, ptr, pos, (int32_t)size);
	return(r);
}

/**
 * benchmark macros
 */
#ifdef BENCH
#include <sys/time.h>

/**
 * @struct _bench
 * @brief benchmark variable container
 */
struct _bench {
	struct timeval s;		/** start */
	int64_t a;				/** accumulator */
};
typedef struct _bench bench_t;

/**
 * @macro bench_init
 */
#define bench_init(b) { \
	memset(&(b).s, 0, sizeof(struct timeval)); \
	(b).a = 0; \
}

/**
 * @macro bench_start
 */
#define bench_start(b) { \
	gettimeofday(&(b).s, NULL); \
}

/**
 * @macro bench_end
 */
#define bench_end(b) { \
	struct timeval e; \
	gettimeofday(&e, NULL); \
	(b).a += ( (e.tv_sec  - (b).s.tv_sec ) * 1000000000 \
	         + (e.tv_usec - (b).s.tv_usec) * 1000); \
}

/**
 * @macro bench_get
 */
#define bench_get(b) ( \
	(b).a \
)

#else /* #ifdef BENCH */

/** disable bench */
struct _bench {};
typedef struct _bench bench_t;
#define bench_init(b) 		;
#define bench_start(b) 		;
#define bench_end(b)		;
#define bench_get(b)		( 0LL )

#endif /* #ifdef BENCH */

#endif /* #ifndef _UTIL_H_INCLUDED */

/*
 * end of util.h
 */
