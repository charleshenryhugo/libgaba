
/**
 * @file gaba.c
 *
 * @brief libgaba (libsea3) API implementations
 *
 * @author Hajime Suzuki
 * @date 2016/1/11
 * @license Apache v2
 */
#include <stdint.h>
#include "gaba.h"
#include "arch/arch.h"

/* aliasing vector macros */
#define _VECTOR_ALIAS_PREFIX	v32i8
#include "arch/vector_alias.h"

/* import utils */
#include "util/util.h"

/* import unittest */
#define UNITTEST_UNIQUE_ID			33

#ifdef TEST
/* use auto-generated main function to run tests */
#define UNITTEST 					1
#define UNITTEST_ALIAS_MAIN			1
#endif

#include  "util/unittest.h"


/* constants */
#define BW 							( 32 )
#define BLK 						( 32 )
#define MIN_BULK_BLOCKS				( 32 )
#define MEM_ALIGN_SIZE				( 32 )		/* 32byte aligned for AVX2 environments */
#define MEM_INIT_SIZE				( (uint64_t)32 * 1024 * 1024 )
#define MEM_MARGIN_SIZE				( 2048 )
#define PSUM_BASE					( 1 )

/* forward declarations */
static int32_t gaba_dp_add_stack(struct gaba_dp_context_s *this);
static void *gaba_dp_malloc(struct gaba_dp_context_s *this, uint64_t size);
static void *gaba_dp_smalloc(struct gaba_dp_context_s *this, uint64_t size);
// static void gaba_dp_free(struct gaba_dp_context_s *this, void *ptr);


/**
 * @macro _match
 * @brief alias to sequence matcher macro
 */
#ifndef _match
// #define _match			_eq 		/* for ASCII encoded sequence */
#define _match		_or 		/* for 2bit encoded */
// #define _match		_and		/* for 4bit encoded */
#endif /* _match */

/**
 * @macro _likely, _unlikely
 * @brief branch prediction hint for gcc-compatible compilers
 */
#define _likely(x)		__builtin_expect(!!(x), 1)
#define _unlikely(x)	__builtin_expect(!!(x), 0)

/**
 * @macro _force_inline
 * @brief inline directive for gcc-compatible compilers
 */
#define _force_inline	inline
// #define _force_inline

/**
 * @macro _load_sc
 * @brief load constant vectors from gaba_dp_context_s
 */
#define _load_sc(this, name)	( _bc_v16i8(_load_v16i8((this)->scv.name)) )

/* direction macros */
#define DYNAMIC 		( 1 )
#if DYNAMIC
#define direction_prefix 		dynamic_

/* direction determiners for the dynamic band algorithms */
/**
 * @macro _dir_fetch
 */
#define _dir_fetch(_dir) { \
	(_dir).dynamic.array <<= 1; \
	(_dir).dynamic.array |= (uint32_t)((_dir).dynamic.acc < 0); \
	debug("fetched dir(%x), %s", (_dir).dynamic.array, _dir_is_down(dir) ? "go down" : "go right"); \
}
/**
 * @macro _dir_update
 * @brief update direction determiner for the next band
 */
#define _dir_update(_dir, _vector, _sign) { \
	(_dir).dynamic.acc += (_sign) * (_ext(_vector, 0) - _ext(_vector, BW-1)); \
	/*debug("acc(%d), (%d, %d)", (_dir).dynamic.acc, _ext(_vector, 0), _ext(_vector, BW-1));*/ \
}
/**
 * @macro _dir_adjust_remainder
 * @brief adjust direction array when termination is detected in the middle of the block
 */
#define _dir_adjust_remainder(_dir, _filled_count) { \
	debug("adjust remainder, array(%x), shifted array(%x)", \
		(_dir).dynamic.array, \
		(_dir).dynamic.array<<(BLK - (_filled_count))); \
	(_dir).dynamic.array <<= (BLK - (_filled_count)); \
}
/**
 * @macro _dir_test_bound, _dir_test_bound_cap
 * @brief test if the bound of the direction array is invaded
 */
#define _dir_test_bound(_dir, k, p) 		( /* nothing to do */ )
#define _dir_test_bound_cap(_dir, k, p)		( /* nothing to do */ )
/**
 * @macro _dir_is_down, _dir_is_right
 * @brief direction indicator (_dir_is_down returns true if dir == down)
 */
#define _dir_is_down(_dir)					( (_dir).dynamic.array & 0x01 )
#define _dir_is_right(_dir)					( !_dir_is_down(_dir) )
/**
 * @macro _dir_load
 */
#define _dir_load(_blk, _local_idx) ({ \
	union gaba_dir_u _dir = (_blk)->dir; \
	debug("load dir idx(%d), mask(%x), shifted mask(%x)", (int32_t)_local_idx, _dir.dynamic.array, _dir.dynamic.array>>(BLK - (_local_idx) - 1)); \
	_dir.dynamic.array >>= (BLK - (_local_idx) - 1); \
	_dir; \
})
/**
 * @macro _dir_bcnt
 */
#define _dir_bcnt(_dir) ( \
	popcnt((_dir).dynamic.array) \
)
/**
 * @macro _dir_windback
 */
#define _dir_windback(_dir) { \
	(_dir).dynamic.array >>= 1; \
}

#else

#define direction_prefix 		guided_

/* direction determiners for the guided band algorithms */
#define _dir_fetch(_dir)					{ /* nothing to do */ }
#define _dir_update(_dir, _vector) { \
	(_dir).guided.ptr++; \
}
#define _dir_adjust_remainder(_dir, i)		{ /* nothing to do */ }
#define _dir_test_bound(_dir, k, p) ( \
	((k)->tdr - (_dir).guided.ptr + 2) - (p) - BLK \
)
#define _dir_test_bound_cap(_dir, k, p) ( \
	((k)->tdr - (_dir).guided.ptr + 2) - (p) \
)
#define _dir_is_down(_dir)					( *(_dir).guided.ptr != 0 )
#define _dir_is_right(_dir)					( *(_dir).guided.ptr == 0 )
#define _dir_load(_blk, _local_idx) ({ \
	union gaba_dir_u _dir = (_blk)->dir; \
	_dir.guided.ptr -= (BLK - (_local_idx)); \
	_dir; \
})
#define _dir_windback(_dir) { \
	(_dir).guided.ptr--; \
}

#endif

/**
 * seqreader macros
 */
#define _rd_tail(k)		_pv_v2i64(&(k)->rr.atail)
#define _rd_len(k)		_pv_v2i32(&(k)->rr.alen)

#define _rd_bufa_base(k)		( (k)->rr.bufa + BLK + BW )
#define _rd_bufb_base(k)		( (k)->rr.bufb )
#define _rd_bufa(k, pos, len)	( _rd_bufa_base(k) - (pos) - (len) )
#define _rd_bufb(k, pos, len)	( _rd_bufb_base(k) + (pos) )
#define _lo64(v)		_ext_v2i64(v, 0)
#define _hi64(v)		_ext_v2i64(v, 1)
#define _lo32(v)		_ext_v2i32(v, 0)
#define _hi32(v)		_ext_v2i32(v, 1)

#define _id(x)		( _ext_v2i32(_cast_v2i64_v2i32(x), 0) )
#define _len(x)		( _ext_v2i32(_cast_v2i64_v2i32(x), 1) )
#define _pos(x)		( _ext_v2i64(x, 1) )


/**
 * @fn transpose_section_pair
 */
static _force_inline
struct gaba_trans_section_s {
	v2i32_t id;
	v2i32_t len;
	v2i64_t base;
} transpose_section_pair(
	v2i64_t a,
	v2i64_t b)
{
	_print_v2i64(a);
	_print_v2i64(b);

	/* transpose */
	v2i32_t id_len_a = _cast_v2i64_v2i32(a);
	v2i32_t id_len_b = _cast_v2i64_v2i32(b);

	_print_v2i32(id_len_a);
	_print_v2i32(id_len_b);

	v2i32_t id = _lo_v2i32(id_len_a, id_len_b);
	v2i32_t len = _hi_v2i32(id_len_a, id_len_b);
	v2i64_t base = _hi_v2i64(a, b);

	_print_v2i32(id);
	_print_v2i32(len);
	_print_v2i64(base);

	return((struct gaba_trans_section_s){
		.id = id,
		.len = len,
		.base = base
	});
}

/*
 * @fn fill_load_section
 */
static _force_inline
void fill_load_section(
	struct gaba_dp_context_s *this,
	struct gaba_section_s const *a,
	struct gaba_section_s const *b,
	uint64_t plim)
{
	/* load current section lengths */
	struct gaba_trans_section_s c = transpose_section_pair(
		_loadu_v2i64(a), _loadu_v2i64(b));

	/* convert current section to (pos of the end, len) */
	v2i64_t c_tail = _add_v2i64(c.base, _cvt_v2i32_v2i64(c.len));

	_print_v2i32(c.id);
	_print_v2i32(c.len);
	_print_v2i64(c_tail);

	/* store sections */
	_store_v2i64(_rd_tail(this), c_tail);
	_store_v2i32(_rd_len(this), c.len);
	this->rr.plim = plim;

	return;
}

/**
 * @fn fill_store_section
 */
static _force_inline
struct gaba_joint_tail_s *fill_store_section(
	struct gaba_joint_tail_s *tail,
	struct gaba_section_s const *a,
	struct gaba_section_s const *b)
{
	_store_v2i64(&tail->a, _loadu_v2i64(a));
	_store_v2i64(&tail->b, _loadu_v2i64(b));
	return(tail);
}

/**
 * @struct gaba_joint_block_s
 * @brief result container for block fill functions
 */
struct gaba_joint_block_s {
	struct gaba_block_s *blk;
	int64_t p;
	int32_t stat;
};

/**
 * @fn fill_bulk_fetch
 *
 * @brief fetch 32bases from current section
 */
static _force_inline
void fill_bulk_fetch(
	struct gaba_dp_context_s *this,
	struct gaba_block_s *blk)
{
	/* load sequence from previous block */
	vec_t const mask = _set(0x0f);
	vec_t w = _load(&(blk - 1)->ch.w);
	vec_t a = _and(mask, w);
	vec_t b = _and(mask, _shr(w, 4));

	_print(w);
	_print(a);
	_print(b);

	debug("atail(%lld), aridx(%d)", this->rr.atail, (blk-1)->aridx);
	debug("btail(%lld), bridx(%d)", this->rr.btail, (blk-1)->bridx);

	/* fetch seq a */
	#define _ra(x)		( rev((x), this->rr.p.alen) )
	rd_load(this->r.loada,
		_rd_bufa(this, BW, BLK),			/* dst */
		this->rr.p.a,						/* src */
		_ra(this->rr.atail - (blk - 1)->aridx),/* pos */
		this->rr.p.alen,					/* lim */
		BLK);								/* len */
	#undef _ra
	_store(_rd_bufa(this, 0, BW), a);

	/* fetch seq b */
	_store(_rd_bufb(this, 0, BW), b);
	rd_load(this->r.loadb,
		_rd_bufb(this, BW, BLK),			/* dst */
		this->rr.p.b,						/* src */
		this->rr.btail - (blk - 1)->bridx,	/* pos */
		this->rr.p.blen,					/* lim */
		BLK);								/* len */

	// return(_seta_v2i32(BLK, BLK));
	return;
}

/**
 * @fn fill_cap_fetch
 *
 * @return the length of the sequence fetched.
 */
static _force_inline
void fill_cap_fetch(
	struct gaba_dp_context_s *this,
	struct gaba_block_s *blk)
{
	/* const */
	v2i32_t const z = _zero_v2i32();
	v2i32_t const tot = _set_v2i32(BLK);

	/* load lengths */
	v2i32_t ridx = _load_v2i32(&(blk - 1)->aridx);

	/* clip len with max load length */
	v2i32_t len = _max_v2i32(_min_v2i32(ridx, tot), z);

	/* load sequence from previous block */
	vec_t const mask = _set(0x0f);
	vec_t w = _load(&(blk - 1)->ch.w);
	vec_t a = _and(mask, w);
	vec_t b = _and(mask, _shr(w, 4));

	_print(w);
	_print(a);
	_print(b);

	/* fetch seq a */
	#define _ra(x)		( rev((x), this->rr.p.alen) )
	rd_load(this->r.loada,
		_rd_bufa(this, BW, _lo32(len)),		/* dst */
		this->rr.p.a,						/* src */
		_ra(this->rr.atail - _lo32(ridx)),	/* pos */
		this->rr.p.alen,					/* lim */
		_lo32(len));						/* len */
	#undef _ra
	_store(_rd_bufa(this, 0, BW), a);

	/* fetch seq b */
	_store(_rd_bufb(this, 0, BW), b);
	rd_load(this->r.loadb,
		_rd_bufb(this, BW, _hi32(len)),		/* dst */
		this->rr.p.b,						/* src */
		this->rr.btail - _hi32(ridx),		/* pos */
		this->rr.p.blen,					/* lim */
		_hi32(len));						/* len */

	// return(len);
	return;
}

/**
 * @macro fill_init_fetch
 * @brief similar to cap fetch, updating ridx and rem
 */
static _force_inline
struct gaba_joint_block_s fill_init_fetch(
	struct gaba_dp_context_s *this,
	struct gaba_phantom_block_s *blk,
	struct gaba_joint_tail_s const *prev_tail,
	v2i32_t ridx)
{
	/* restore (brem, arem) */
	int64_t prem = -prev_tail->psum;
	int64_t arem = (prem + 1) / 2;
	int64_t brem = prem / 2;
	v2i32_t rem = _seta_v2i32(brem, arem);

	/* calc the next rem */
	v2i32_t const z = _zero_v2i32();
	// v2i32_t const ofs = _seta_v2i32(0, 1);
	v2i32_t const ofs = _seta_v2i32(1, 0);
	v2i32_t nrem = _max_v2i32(_sub_v2i32(rem, ridx), z);
	v2i32_t rrem = _sub_v2i32(_swap_v2i32(nrem), ofs);
	nrem = _max_v2i32(nrem, rrem);

	/* cap fetch */
	v2i32_t len = _sub_v2i32(rem, nrem);

	/* load sequence from the previous block */ {
		struct gaba_block_s *prev_blk = _last_block(prev_tail);
		vec_t const mask = _set(0x0f);
		vec_t w = _load(&prev_blk->ch.w);
		vec_t a = _and(mask, w);
		vec_t b = _and(mask, _shr(w, 4));

		debug("prev_blk(%p), prev_tail(%p)", prev_blk, prev_tail);
		_print(w);
		_print(a);
		_print(b);

		/* fetch seq a */
		#define _ra(x)		( rev((x), this->rr.p.alen) )
		rd_load(this->r.loada,
			_rd_bufa(this, BW, _lo32(len)),		/* dst */
			this->rr.p.a,						/* src */
			_ra(this->rr.atail - _lo32(ridx)),	/* pos */
			this->rr.p.alen,					/* lim */
			_lo32(len));						/* len */
		#undef _ra
		_store(_rd_bufa(this, 0, BW), a);

		/* fetch seq b */
		_store(_rd_bufb(this, 0, BW), b);
		rd_load(this->r.loadb,
			_rd_bufb(this, BW, _hi32(len)),		/* dst */
			this->rr.p.b,						/* src */
			this->rr.btail - _hi32(ridx),		/* pos */
			this->rr.p.blen,					/* lim */
			_hi32(len));						/* len */
	}

	/* store char vector to the current block */ {
		vec_t a = _loadu(_rd_bufa(this, _lo32(len), BW));
		vec_t b = _loadu(_rd_bufb(this, _hi32(len), BW));
		_store(&blk->ch.w, _or(a, _shl(b, 4)));

		_print(a);
		_print(b);
	}

	/* adjust and store ridx */
	_print_v2i32(ridx);
	_print_v2i32(len);
	ridx = _sub_v2i32(ridx, len);
	_store_v2i32(&blk->aridx, ridx);
	_print_v2i32(ridx);
	debug("blk(%p), p(%d), stat(%x)", blk + 1, _lo32(len) + _hi32(len), _mask_v2i32(_eq_v2i32(ridx, z)));

	return((struct gaba_joint_block_s){
		.blk = (struct gaba_block_s *)(blk + 1),
		.p = _lo32(len) + _hi32(len),
		.stat = (_mask_v2i32(_eq_v2i32(ridx, z)) == V2I32_MASK_00) ? CONT : UPDATE
	});
}

/**
 * @macro fill_restore_fetch
 * @brief fetch sequence from existing block
 */
static _force_inline
void fill_restore_fetch(
	struct gaba_dp_context_s *this,
	struct gaba_block_s const *blk)
{
	vec_t const mask = _set(0x0f);

	/* calc cnt */
	v2i32_t curr_len = _load_v2i32(&blk->aridx);
	v2i32_t prev_len = _load_v2i32(&(blk - 1)->aridx);
	v2i32_t cnt = _sub_v2i32(prev_len, curr_len);

	/* from the current block */
	vec_t cw = _load(&blk->ch.w);
	vec_t ca = _and(mask, cw);
	vec_t cb = _and(mask, _shr(cw, 4));
	_storeu(_rd_bufa(this, _lo32(cnt), BW), ca);
	_storeu(_rd_bufb(this, _hi32(cnt), BW), cb);

	/* from the previous block */
	vec_t pw = _load(&(blk - 1)->ch.w);
	vec_t pa = _and(mask, pw);
	vec_t pb = _and(mask, _shr(pw, 4));
	_store(_rd_bufa(this, 0, BW), pa);
	_store(_rd_bufb(this, 0, BW), pb);

	_print(pa);
	_print(pb);

	return;
}

/**
 * @macro fill_update_section
 */
static _force_inline
v2i32_t fill_update_section(
	struct gaba_dp_context_s *this,
	struct gaba_block_s *blk,
	v2i32_t cnt)
{
	/* update pos and ridx */
	v2i32_t ridx = _load_v2i32(&(blk - 1)->aridx);
	ridx = _sub_v2i32(ridx, cnt);
	_print_v2i32(ridx);

	/* store ridx to block */
	_store_v2i32(&blk->aridx, ridx);

	vec_t a = _loadu(_rd_bufa(this, _lo32(cnt), BW));
	vec_t b = _loadu(_rd_bufb(this, _hi32(cnt), BW));
	_store(&blk->ch.w, _or(a, _shl(b, 4)));

	_print(a);
	_print(b);
	return(ridx);
}

/**
 * @fn fill_create_phantom_block
 * @brief create joint_head on the stack to start block extension
 */
static _force_inline
struct gaba_joint_block_s fill_create_phantom_block(
	struct gaba_dp_context_s *this,
	struct gaba_joint_tail_s const *prev_tail)
{
	debug("create head prev_tail(%p), p(%d), psum(%lld), ssum(%d)",
		prev_tail, prev_tail->p, prev_tail->psum, prev_tail->ssum);

	/* init working stack */
	struct gaba_phantom_block_s *blk = (struct gaba_phantom_block_s *)this->stack_top;
	struct gaba_block_s *pblk = _last_block(prev_tail);

	/* copy phantom vectors from the previous fragment */
	_memcpy_blk_aa(&blk->diff, &pblk->diff, offsetof(struct gaba_phantom_block_s, sd.max));

	/* fill max vector with zero */
	_store(&blk->sd.max, _zero());

	/* copy remaining */
	blk->dir = pblk->dir;
	blk->offset = pblk->offset;

	/* calc ridx */
	v2i32_t ridx = _sub_v2i32(
		_load_v2i32(_rd_len(this)),
		_load_v2i32(&prev_tail->apos));
	_print_v2i32(ridx);

	/* check if init fetch is needed */
	if(prev_tail->psum >= 0) {
		/* store index on the current section */
		_store_v2i32(&blk->aridx, ridx);
		
		/* copy char vectors from prev_tail */
		_store(&blk->ch, _load(&pblk->ch));

		return((struct gaba_joint_block_s){
			.blk = (struct gaba_block_s *)(blk + 1),
			.p = 0,
			.stat = CONT
		});
	} else {
		/* init fetch */
		return(fill_init_fetch(this, blk, prev_tail, ridx));
	}
}

/**
 * @fn fill_create_tail
 * @brief create joint_tail at the end of the blocks
 */
static _force_inline
struct gaba_joint_tail_s *fill_create_tail(
	struct gaba_dp_context_s *this,
	struct gaba_joint_tail_s const *prev_tail,
	struct gaba_block_s *blk,
	int64_t p,
	int32_t stat)
{
	debug("create tail, p(%lld)", p);

	/* create joint_tail */
	struct gaba_joint_tail_s *tail = (struct gaba_joint_tail_s *)blk;
	this->stack_top = (void *)(tail + 1);	/* write back stack_top */
	tail->v = prev_tail->v;					/* copy middle deltas */

	/* update p */
	int64_t prev_psum = prev_tail->psum;
	int32_t np = (prev_psum < 0) ? MAX2(p + prev_psum, 0) : p;

	/* save misc to joint_tail */
	tail->psum = p + prev_tail->psum;
	tail->p = np;
	debug("p(%lld), prev_tail->psum(%lld), prev_tail->p(%d), tail->psum(%lld), tail->p(%d)",
		p, prev_tail->psum, prev_tail->p, tail->psum, tail->p);
	tail->ssum = prev_tail->ssum + 1;
	tail->tail = prev_tail;					/* to treat tail chain as linked list */
	tail->rem_len = 0;

	/* search max section */
	v32i16_t sd = _cvt_v32i8_v32i16(_load(&(blk - 1)->sd.max));
	v32i16_t md = _load_v32i16(prev_tail->v);
	_print_v32i16(sd);
	_print_v32i16(md);

	/* extract max */
	md = _add_v32i16(md, sd);
	int16_t max = _hmax_v32i16(md);
	_print_v32i16(md);

	/* store */
	// tail->mask_max.mask = mask_max;
	tail->max = max + (blk - 1)->offset;

	debug("offset(%lld)", (blk - 1)->offset);
	debug("max(%d)", _hmax_v32i16(md));
	// debug("mask_max(%u)", tail->mask_max.all);

	/* store section */
	v2i32_t const z = _zero_v2i32();
	v2i32_t ridx = _load_v2i32(&(blk - 1)->aridx);
	v2i32_t len = _load_v2i32(_rd_len(this));
	_print_v2i32(ridx);
	_print_v2i32(len);
	_print_v2i32(_sel_v2i32(
		_eq_v2i32(ridx, z),
		z, _sub_v2i32(len, ridx)));
	_store_v2i32(&tail->apos, _sel_v2i32(_eq_v2i32(ridx, z),
		z, _sub_v2i32(len, ridx)));

	/* store status */
	tail->stat = stat | _mask_v2i32(_eq_v2i32(ridx, z));
	return(tail);
}

/**
 * @macro _fill_load_contexts
 * @brief load vectors onto registers
 */
#define _fill_load_contexts(_blk) \
	debug("blk(%p)", (_blk)); \
	/* load sequence buffer offset */ \
	uint8_t *aptr = _rd_bufa(this, 0, BW); \
	uint8_t *bptr = _rd_bufb(this, 0, BW); \
	/* load mask pointer */ \
	/*union gaba_mask_pair_u *mask_ptr = (_blk)->mask;*/ \
	union gaba_mask_pair_u *mask_ptr = (_blk)->mask; \
	/* load vector registers */ \
	vec_t const mask = _set(0x07); \
	vec_t register dh = _load(_pv(((_blk) - 1)->diff.dh)); \
	vec_t register dv = _load(_pv(((_blk) - 1)->diff.dv)); \
	vec_t register de = _and(mask, dh); \
	vec_t register df = _and(mask, dv); \
	dh = _shr(_andn(mask, dh), 3); \
	dv = _shr(_andn(mask, dv), 3); \
	de = _add(dv, de); \
	df = _add(dh, df); \
	dh = _sub(_zero(), dh); \
	_print(_add(dh, _load_sc(this, ofsh))); \
	_print(_add(dv, _load_sc(this, ofsv))); \
	_print(_sub(_sub(de, dv), _load_sc(this, adjh))); \
	_print(_sub(_add(df, dh), _load_sc(this, adjv))); \
	/* load delta vectors */ \
	vec_t register delta = _load(_pv(((_blk) - 1)->sd.delta)); \
	vec_t register max = _load(_pv(((_blk) - 1)->sd.max)); \
	_print(max); \
	_print_v32i16(_add_v32i16(_cvt_v32i8_v32i16(delta), _load_v32i16(this->tail->v))); \
	/* load direction determiner */ \
	union gaba_dir_u dir = ((_blk) - 1)->dir; \
	/* load large offset */ \
	int64_t offset = ((_blk) - 1)->offset; \
	debug("offset(%lld)", offset);

/**
 * @macro _fill_body
 * @brief update vectors
 */
#define _fill_body() { \
	vec_t register t = _match(_loadu(aptr), _loadu(bptr)); \
	_print(_loadu(aptr)); \
	_print(_loadu(bptr)); \
	t = _shuf(_load_sc(this, sb), t); \
	_print(t); \
	/*_print(_load_sc(this, sb));*/ \
	t = _max(de, t); \
	t = _max(df, t); \
	mask_ptr->pair.h.mask = _mask(_eq(t, de)); \
	mask_ptr->pair.v.mask = _mask(_eq(t, df)); \
	debug("mask(%llx)", mask_ptr->all); \
	mask_ptr++; \
	/*mask_ptr++->mask = _mask(_eq(t, df));*/ \
	df = _sub(_max(_add(df, _load_sc(this, adjv)), t), dv); \
	dv = _sub(dv, t); \
	de = _add(_max(_add(de, _load_sc(this, adjh)), t), dh); \
	t = _add(dh, t); \
	dh = dv; dv = t; \
	_print(_add(dh, _load_sc(this, ofsh))); \
	_print(_add(dv, _load_sc(this, ofsv))); \
	_print(_sub(_sub(de, dv), _load_sc(this, adjh))); \
	_print(_sub(_add(df, dh), _load_sc(this, adjv))); \
}

/**
 * @macro _fill_update_delta
 * @brief update small delta vector and max vector
 */
#define _fill_update_delta(_op, _vector, _offset, _sign) { \
	delta = _op(delta, _add(_vector, _offset)); \
	/*_print(delta);*/ \
	max = _max(max, delta); \
	/*_print(max);*/ \
	_dir_update(dir, _vector, _sign); \
	_print_v32i16(_add_v32i16(_set_v32i16(offset), _add_v32i16(_cvt_v32i8_v32i16(delta), _load_v32i16(this->tail->v)))); \
	_print_v32i16(_add_v32i16(_set_v32i16(offset), _add_v32i16(_cvt_v32i8_v32i16(max), _load_v32i16(this->tail->v)))); \
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
#define _fill_right() { \
	dh = _bsl(dh, 1);	/* shift left dh */ \
	df = _bsl(df, 1);	/* shift left df */ \
	_fill_body();		/* update vectors */ \
	_fill_update_delta(_sub, dh, _load_sc(this, ofsh), -1); \
}
#define _fill_down_update_ptr() { \
	bptr++;				/* increment sequence buffer pointer */ \
}
#define _fill_down_windback_ptr() { \
	bptr--; \
}
#define _fill_down() { \
	dv = _bsr(dv, 1);	/* shift right dv */ \
	de = _bsr(de, 1);	/* shift right de */ \
	_fill_body();		/* update vectors */ \
	_fill_update_delta(_add, dv, _load_sc(this, ofsv), 1); \
}

/**
 * @macro _fill_update_offset
 * @brief update offset and max vector, reset the small delta
 */
#define _fill_update_offset() { \
	int8_t _cd = _ext(delta, BW/2); \
	offset += _cd; \
	delta = _sub(delta, _set(_cd)); \
	max = _sub(max, _set(_cd)); \
	/*debug("_cd(%d), offset(%lld)", _cd, offset);*/ \
}

/**
 * @macro _fill_store_vectors
 * @brief store vectors at the end of the block
 */
#define _fill_store_vectors(_blk) ({ \
	/* store diff vectors */ \
	de = _sub(de, dv); \
	df = _add(df, dh); \
	dh = _sub(_zero(), dh); \
	_print(dh); \
	_print(dv); \
	_print(de); \
	_print(df); \
	dh = _shl(dh, 3); \
	dv = _shl(dv, 3); \
	_print(dh); \
	_print(dv); \
	_store(_pv((_blk)->diff.dh), _add(dh, de)); \
	_store(_pv((_blk)->diff.dv), _add(dv, df)); \
	_print(_add(dh, de)); \
	_print(_add(dv, df)); \
	/* store delta vectors */ \
	_store(_pv((_blk)->sd.delta), delta); \
	_store(_pv((_blk)->sd.max), max); \
	/* store direction array */ \
	(_blk)->dir = dir; \
	/* store large offset */ \
	(_blk)->offset = offset; \
	/* calc cnt */ \
	uint64_t acnt = _rd_bufa(this, 0, BW) - aptr; \
	uint64_t bcnt = bptr - _rd_bufb(this, 0, BW); \
	/*_print_v2i32(_seta_v2i32(bcnt, acnt));*/ \
	_seta_v2i32(bcnt, acnt); \
})

/**
 * @fn fill_test_xdrop
 * @brief returns negative if terminate-condition detected
 */
static _force_inline
int64_t fill_test_xdrop(
	struct gaba_dp_context_s const *this,
	struct gaba_block_s const *blk)
{
	return(this->tx - blk->sd.max[BW/2]);
}

/**
 * @fn fill_bulk_test_seq_bound
 * @brief returns negative if ij-bound (for the bulk fill) is invaded
 */
static _force_inline
int64_t fill_bulk_test_seq_bound(
	struct gaba_dp_context_s const *this,
	struct gaba_block_s const *blk)
{
	debug("test(%lld, %lld), len(%d, %d)",
		(int64_t)blk->aridx - BW,
		(int64_t)blk->bridx - BW,
		blk->aridx, blk->bridx);
	return(((int64_t)blk->aridx - BW)
		 | ((int64_t)blk->bridx - BW));
}

/**
 * @fn fill_cap_test_seq_bound
 * @brief returns negative if ij-bound (for the cap fill) is invaded
 */
#define _fill_cap_test_seq_bound_init(_blk) \
	uint8_t *alim = _rd_bufa(this, ((_blk) - 1)->aridx, BW); \
	uint8_t *blim = _rd_bufb(this, ((_blk) - 1)->bridx, BW);
#define _fill_cap_test_seq_bound() ( \
	((int64_t)aptr - (int64_t)alim) | ((int64_t)blim - (int64_t)bptr) \
)

/**
 * @fn fill_bulk_test_p_bound
 * @brief returns negative if p-bound (for the bulk fill) is invaded
 */
static _force_inline
int64_t fill_bulk_test_p_bound(
	struct gaba_dp_context_s const *this,
	int64_t p)
{
	return(this->rr.plim - p - BW);
}
static _force_inline
int64_t fill_cap_test_p_bound(
	struct gaba_dp_context_s const *this,
	int64_t p)
{
	return(this->rr.plim - p);
}

/**
 * @fn fill_bulk_block
 * @brief fill a block
 */
static _force_inline
void fill_bulk_block(
	struct gaba_dp_context_s *this,
	struct gaba_block_s *blk)
{
	/* fetch sequence */
	fill_bulk_fetch(this, blk);

	/* load vectors onto registers */
	debug("blk(%p)", blk);
	_fill_load_contexts(blk);
	/**
	 * @macro _fill_block
	 * @brief an element of unrolled fill-in loop
	 */
	#define _fill_block(_direction, _label, _jump_to) { \
		_dir_fetch(dir); \
		if(_unlikely(!_dir_is_##_direction(dir))) { \
			goto _linear_fill_##_jump_to; \
		} \
		_linear_fill_##_label: \
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

	/* update seq offset */
	_fill_update_offset();

	/* store vectors */
	v2i32_t cnt = _fill_store_vectors(blk);

	/* update section */
	fill_update_section(this, blk, cnt);

	return;
}

/**
 * @fn fill_bulk_predetd_blocks
 * @brief fill <blk_cnt> contiguous blocks without ij-bound test
 */
static _force_inline
struct gaba_joint_block_s fill_bulk_predetd_blocks(
	struct gaba_dp_context_s *this,
	struct gaba_block_s *blk,
	uint64_t blk_cnt)
{
	int32_t stat = CONT;
	uint64_t bc = 0;
	for(bc = 0; bc < blk_cnt; bc++) {
		/* check xdrop termination */
		if(fill_test_xdrop(this, blk - 1) < 0) {
			stat = TERM; break;
		}

		/* bulk fill */
		debug("blk(%p)", blk);
		fill_bulk_block(this, blk++);
	}
	return((struct gaba_joint_block_s){
		.blk = blk,
		.p = (int64_t)bc * BLK,
		.stat = stat
	});
}

/**
 * @fn fill_bulk_seq_bounded
 * @brief fill blocks with ij-bound test
 */
static _force_inline
struct gaba_joint_block_s fill_bulk_seq_bounded(
	struct gaba_dp_context_s *this,
	struct gaba_block_s *blk)
{
	int32_t stat = CONT;

	/* init local coordinate */
	int64_t p = 0;

	/* bulk fill loop */
	while(1) {
		/* check termination */
		if((fill_test_xdrop(this, blk - 1)
		  | fill_bulk_test_seq_bound(this, blk - 1)) < 0) {
			break;
		}
		
		/* bulk fill */
		debug("blk(%p)", blk);
		fill_bulk_block(this, blk++);
		
		/* update p-coordinate */
		p += BLK;
	}
	if(fill_test_xdrop(this, blk - 1) < 0) { stat = TERM; }
	return((struct gaba_joint_block_s){
		.blk = blk,
		.p = p,
		.stat = stat 		/* < 0 if stat == TERM */
	});
}

/**
 * @fn fill_bulk_seq_p_bounded
 * @brief fill blocks with ij-bound and p-bound test
 */
static _force_inline
struct gaba_joint_block_s fill_bulk_seq_p_bounded(
	struct gaba_dp_context_s *this,
	struct gaba_block_s *blk)
{
	int32_t stat = CONT;

	/* init local coordinate */
	int64_t p = 0;

	/* bulk fill loop */
	while(1) {
		/* check termination */
		if((fill_test_xdrop(this, blk - 1)
		  | fill_bulk_test_seq_bound(this, blk - 1)
		  | fill_bulk_test_p_bound(this, p)) < 0) {
			break;
		}
		
		/* bulk fill */
		debug("blk(%p)", blk);
		fill_bulk_block(this, blk++);
		
		/* update p-coordinate */
		p += BLK;
	}
	if(fill_test_xdrop(this, blk - 1) < 0) { stat = TERM; }
	return((struct gaba_joint_block_s){
		.blk = blk,
		.p = p,
		.stat = stat 		/* < 0 if stat == TERM */
	});
}

/**
 * @fn fill_cap_seq_bounded
 * @brief fill blocks with cap test
 */
static _force_inline
struct gaba_joint_block_s fill_cap_seq_bounded(
	struct gaba_dp_context_s *this,
	struct gaba_block_s *blk)
{
	int32_t stat = CONT;
	int64_t p = 0;

	while(1) {
		/* check xdrop termination */
		if(fill_test_xdrop(this, blk - 1) < 0) {
			stat = TERM; goto _fill_cap_seq_bounded_finish;
		}
		/* fetch sequence */
		fill_cap_fetch(this, blk);

		/* vectors on registers inside this block */ {
			_fill_cap_test_seq_bound_init(blk);
			_fill_load_contexts(blk);

			/* update diff vectors */
			uint64_t i = 0;
			for(i = 0; i < BLK; i++) {
				/* determine direction */
				_dir_fetch(dir);

				if(_dir_is_right(dir)) {
					/* update sequence coordinate and then check term */
					_fill_right_update_ptr();
					
					if(_fill_cap_test_seq_bound() < 0) {
						_fill_right_windback_ptr();
						_dir_windback(dir);
						break;
					}

					/* update band */
					_fill_right();
				} else {
					/* update sequence coordinate and then check term */
					_fill_down_update_ptr();
					
					if(_fill_cap_test_seq_bound() < 0) {
						_fill_down_windback_ptr();
						_dir_windback(dir);
						break;
					}

					/* update band */
					_fill_down();
				}
			}
			/* adjust dir remainder */
			_dir_adjust_remainder(dir, i);

			/* update seq offset */
			_fill_update_offset();
			
			/* store mask and vectors */
			v2i32_t cnt = _fill_store_vectors(blk);

			/* update section */
			fill_update_section(this, blk, cnt);

			/* update block pointer and p-coordinate */
			blk += (i != 0); p += i;

			/* break if not filled full length */
			if(i != BLK) { stat = UPDATE; break; }
		}
	}

	debug("blk(%p), p(%lld), stat(%x)", blk, p, stat);

_fill_cap_seq_bounded_finish:;
	return((struct gaba_joint_block_s){
		.blk = blk,
		.p = p,
		.stat = stat
	});
}

/**
 * @fn fill_cap_seq_p_bounded
 * @brief fill blocks with cap test
 */
static _force_inline
struct gaba_joint_block_s fill_cap_seq_p_bounded(
	struct gaba_dp_context_s *this,
	struct gaba_block_s *blk)
{
	int32_t stat = CONT;
	int64_t p = 0;

	while(1) {
		/* check xdrop termination */
		if(fill_test_xdrop(this, blk - 1) < 0) {
			stat = TERM; goto _fill_cap_seq_bounded_finish;
		}
		/* fetch sequence */
		fill_cap_fetch(this, blk);

		/* vectors on registers inside this block */ {
			_fill_cap_test_seq_bound_init(blk);
			_fill_load_contexts(blk);

			/* update diff vectors */
			uint64_t i = 0;
			for(i = 0; i < BLK; i++) {
				/* determine direction */
				_dir_fetch(dir);

				if(_dir_is_right(dir)) {
					/* update sequence coordinate and then check term */
					_fill_right_update_ptr();
					
					if((_fill_cap_test_seq_bound()
					  | fill_cap_test_p_bound(this, ++p)) < 0) {
						_fill_right_windback_ptr();
						_dir_windback(dir);
						break;
					}

					/* update band */
					_fill_right();
				} else {
					/* update sequence coordinate and then check term */
					_fill_down_update_ptr();
					
					if((_fill_cap_test_seq_bound()
					  | fill_cap_test_p_bound(this, ++p)) < 0) {
						_fill_down_windback_ptr();
						_dir_windback(dir);
						break;
					}

					/* update band */
					_fill_down();
				}
			}
			/* adjust dir remainder */
			_dir_adjust_remainder(dir, i);
			
			/* update seq offset */
			_fill_update_offset();
			
			/* store mask and vectors */
			v2i32_t cnt = _fill_store_vectors(blk);

			/* update section */
			fill_update_section(this, blk, cnt);

			/* update block pointer and p-coordinate */
			blk += (i != 0);

			/* break if not filled full length */
			if(i != BLK) { stat = UPDATE; break; }
		}
	}

	debug("blk(%p), p(%lld), stat(%x)", blk, p, stat);

_fill_cap_seq_bounded_finish:;
	return((struct gaba_joint_block_s){
		.blk = blk,
		.p = p,
		.stat = stat
	});
}

/**
 * @fn calc_max_bulk_blocks_mem
 * @brief calculate maximum number of blocks (limited by stack size)
 */
static _force_inline
uint64_t calc_max_bulk_blocks_mem(
	struct gaba_dp_context_s const *this)
{
	uint64_t const rem = sizeof(struct gaba_joint_tail_s)
					   + 3 * sizeof(struct gaba_block_s);
	uint64_t mem_size = this->stack_end - this->stack_top;
	return((mem_size - rem) / sizeof(struct gaba_block_s) / BLK);
}

/**
 * @fn calc_max_bulk_blocks_seq
 * @brief calculate maximum number of blocks (limited by seq bounds)
 */
static _force_inline
uint64_t calc_max_bulk_blocks_seq_blk(
	struct gaba_dp_context_s const *this,
	struct gaba_block_s const *blk)
{
	uint64_t max_bulk_p = MIN2(
		(blk - 1)->aridx,
		(blk - 1)->bridx);
	return(max_bulk_p / BLK);
}
static _force_inline
uint64_t calc_max_bulk_blocks_seq_tail(
	struct gaba_dp_context_s const *this,
	struct gaba_joint_tail_s const *tail)
{
	uint64_t max_bulk_p = MIN2(
		this->rr.alen - tail->apos,
		this->rr.blen - tail->bpos);
	return(max_bulk_p / BLK);
}

/**
 * @fn calc_max_bulk_blocks_seq_p
 * @brief calculate maximum number of blocks (limited by seq bounds)
 */
static _force_inline
uint64_t calc_max_bulk_blocks_seq_p_blk(
	struct gaba_dp_context_s const *this,
	struct gaba_block_s const *blk,
	int64_t psum)
{
	uint64_t max_bulk_p = MIN3(
		(blk - 1)->aridx,
		(blk - 1)->bridx,
		this->rr.plim - psum);
	return(max_bulk_p / BLK);
}
static _force_inline
uint64_t calc_max_bulk_blocks_seq_p_tail(
	struct gaba_dp_context_s const *this,
	struct gaba_joint_tail_s const *tail,
	int64_t psum)
{
	uint64_t max_bulk_p = MIN3(
		this->rr.alen - tail->apos,
		this->rr.blen - tail->bpos,
		this->rr.plim - psum);
	return(max_bulk_p / BLK);
}

/**
 * @fn fill_mem_bounded
 * @brief fill <blk_cnt> contiguous blocks without seq bound tests, adding head and tail
 */
static _force_inline
struct gaba_joint_tail_s *fill_mem_bounded(
	struct gaba_dp_context_s *this,
	struct gaba_joint_tail_s const *prev_tail,
	uint64_t blk_cnt)
{
	struct gaba_joint_block_s h = fill_create_phantom_block(this, prev_tail);
	struct gaba_joint_block_s b = fill_bulk_predetd_blocks(this, h.blk, blk_cnt);
	return(fill_create_tail(this, prev_tail, b.blk, h.p + b.p, b.stat));
}

/**
 * @fn fill_seq_bounded
 * @brief fill blocks with seq bound tests, adding head and tail
 */
static _force_inline
struct gaba_joint_tail_s *fill_seq_bounded(
	struct gaba_dp_context_s *this,
	struct gaba_joint_tail_s const *prev_tail)
{
	struct gaba_joint_block_s stat = fill_create_phantom_block(this, prev_tail);
	int64_t psum = stat.p;

	/* check if term detected in init fetch */
	if(stat.stat != CONT) {
		goto _fill_seq_bounded_finish;
	}

	/* calculate block size */
	uint64_t seq_bulk_blocks = calc_max_bulk_blocks_seq_blk(this, stat.blk);
	while(seq_bulk_blocks > MIN_BULK_BLOCKS) {
		/* bulk fill without ij-bound test */
		psum += (stat = fill_bulk_predetd_blocks(this, stat.blk, seq_bulk_blocks)).p;
		if(stat.stat != CONT) {
			goto _fill_seq_bounded_finish;	/* skip cap */
		}
		seq_bulk_blocks = calc_max_bulk_blocks_seq_blk(this, stat.blk);
	}

	/* bulk fill with ij-bound test */
	psum += (stat = fill_bulk_seq_bounded(this, stat.blk)).p;

	if(stat.stat != CONT) {
		goto _fill_seq_bounded_finish;	/* skip cap */
	}

	/* cap fill (without p-bound test) */
	psum += (stat = fill_cap_seq_bounded(this, stat.blk)).p;

_fill_seq_bounded_finish:;
	return(fill_create_tail(this, prev_tail, stat.blk, psum, stat.stat));
}

/**
 * @fn fill_seq_p_bounded
 * @brief fill blocks with seq bound tests, adding head and tail
 */
static _force_inline
struct gaba_joint_tail_s *fill_seq_p_bounded(
	struct gaba_dp_context_s *this,
	struct gaba_joint_tail_s const *prev_tail)
{
	struct gaba_joint_block_s stat = fill_create_phantom_block(this, prev_tail);
	int64_t psum = stat.p;

	/* check if term detected in init fetch */
	if(stat.stat != CONT) {
		goto _fill_seq_p_bounded_finish;
	}

	/* calculate block size */
	uint64_t seq_bulk_blocks = calc_max_bulk_blocks_seq_p_blk(this, stat.blk, psum);
	while(seq_bulk_blocks > MIN_BULK_BLOCKS) {
		/* bulk fill without ij-bound test */
		psum += (stat = fill_bulk_predetd_blocks(this, stat.blk, seq_bulk_blocks)).p;
		if(stat.stat != CONT) {
			goto _fill_seq_p_bounded_finish;	/* skip cap */
		}
		seq_bulk_blocks = calc_max_bulk_blocks_seq_p_blk(this, stat.blk, psum);
	}

	/* bulk fill with ij-bound test */
	psum += (stat = fill_bulk_seq_p_bounded(this, stat.blk)).p;

	if(stat.stat != CONT) {
		goto _fill_seq_p_bounded_finish;	/* skip cap */
	}

	/* cap fill (without p-bound test) */
	psum += (stat = fill_cap_seq_p_bounded(this, stat.blk)).p;

_fill_seq_p_bounded_finish:;
	return(fill_create_tail(this, prev_tail, stat.blk, psum, stat.stat));
}

/**
 * @fn fill_section_seq_bounded
 * @brief fill dp matrix inside section pairs
 */
static _force_inline
struct gaba_joint_tail_s *fill_section_seq_bounded(
	struct gaba_dp_context_s *this,
	struct gaba_joint_tail_s const *prev_tail,
	struct gaba_section_s const *a,
	struct gaba_section_s const *b)
{
	/* init section and restore sequence reader buffer */
	fill_load_section(this, a, b, INT64_MAX);

	/* init tail pointer */
	struct gaba_joint_tail_s *tail = _tail(prev_tail);

	/* calculate block sizes */
	uint64_t mem_bulk_blocks = calc_max_bulk_blocks_mem(this);
	uint64_t seq_bulk_blocks = calc_max_bulk_blocks_seq_tail(this, tail);

	/* extra large bulk fill (with stack allocation) */
	while(_unlikely(mem_bulk_blocks < seq_bulk_blocks)) {
		if(mem_bulk_blocks > MIN_BULK_BLOCKS) {
			if((tail = fill_store_section(
				fill_mem_bounded(this, tail, mem_bulk_blocks),
				a, b))->stat != CONT) {
				return(tail);
			}

			/* fill-in area has changed */
			seq_bulk_blocks = calc_max_bulk_blocks_seq_tail(this, tail);
		}

		/* malloc the next stack and set pointers */
		gaba_dp_add_stack(this);

		/* stack size has changed */
		mem_bulk_blocks = calc_max_bulk_blocks_mem(this);
	}

	debug("v(%p), psum(%lld), p(%d)", tail->v, tail->psum, tail->p);

	/* bulk fill with seq bound check */
	return(fill_store_section(fill_seq_bounded(this, tail), a, b));
}

/**
 * @fn fill_section_seq_p_bounded
 * @brief fill dp matrix inside section pairs
 */
static _force_inline
struct gaba_joint_tail_s *fill_section_seq_p_bounded(
	struct gaba_dp_context_s *this,
	struct gaba_joint_tail_s const *prev_tail,
	struct gaba_section_s const *a,
	struct gaba_section_s const *b,
	int64_t plim)
{
	/* init section and restore sequence reader buffer */
	fill_load_section(this, a, b, plim);

	/* init tail pointer */
	struct gaba_joint_tail_s *tail = _tail(prev_tail);

	/* calculate block sizes */
	uint64_t mem_bulk_blocks = calc_max_bulk_blocks_mem(this);
	uint64_t seq_bulk_blocks = calc_max_bulk_blocks_seq_p_tail(this, tail, 0);

	/* extra large bulk fill (with stack allocation) */
	while(_unlikely(mem_bulk_blocks < seq_bulk_blocks)) {
		if(mem_bulk_blocks > MIN_BULK_BLOCKS) {
			if((tail = fill_store_section(
				fill_mem_bounded(this, tail, mem_bulk_blocks),
				a, b))->stat != CONT) {
				return(tail);
			}

			/* fill-in area has changed */
			seq_bulk_blocks = calc_max_bulk_blocks_seq_p_tail(this, tail, 0);
		}

		/* malloc the next stack and set pointers */
		gaba_dp_add_stack(this);

		/* stack size has changed */
		mem_bulk_blocks = calc_max_bulk_blocks_mem(this);
	}

	debug("v(%p), psum(%lld), p(%d)", tail->v, tail->psum, tail->p);

	/* bulk fill with seq bound check */
	return(fill_store_section(fill_seq_p_bounded(this, tail), a, b));
}

/**
 * @fn gaba_dp_fill_root
 *
 * @brief build_root API
 */
struct gaba_fill_s *gaba_dp_fill_root(
	struct gaba_dp_context_s *this,
	struct gaba_section_s const *a,
	uint32_t apos,
	struct gaba_section_s const *b,
	uint32_t bpos)
{
	/* store section info */
	this->tail->apos = apos;
	this->tail->bpos = bpos;
	_store_v2i64(&this->tail->a, _loadu_v2i64(a));
	_store_v2i64(&this->tail->b, _loadu_v2i64(b));

	return(_fill(fill_section_seq_bounded(this, this->tail, a, b)));
}

/**
 * @fn gaba_dp_fill
 *
 * @brief fill API
 */
struct gaba_fill_s *gaba_dp_fill(
	struct gaba_dp_context_s *this,
	struct gaba_fill_s const *prev_sec,
	struct gaba_section_s const *a,
	struct gaba_section_s const *b)
{
	struct gaba_joint_tail_s const *tail = _tail(prev_sec);

	dump(tail, sizeof(struct gaba_joint_tail_s));

	/* check if the tail is merge_tail */
	if(tail->v == NULL) {
		/* tail is merge_tail */
		if(tail->rem_len > 0) {
			#if 0
			/* aとbをclipする */
			ca = clip_to_bw(a);
			cb = clip_to_bw(b);

			/* バンドに対応するseqが揃うまでfillを続ける */
			new_tail = gaba_dp_smalloc(sizeof(struct gaba_merge_tail_s));
			for(i = 0; i < tail->num_tail; i++) {
				new_tail->tail[i] = fill_section_seq_bounded(
					this, tail->tail[i], ca, cb);
			}

			/* rem_lenを更新し、0になったらpを揃えるモードに移る */
			if(tail->rem_len == 0) {
				tail->prem = ...;
			}
			#endif
		} else {
			#if 0
			/* pが揃うまでfillを続ける */
			new_tail = gaba_dp_smalloc(sizeof(struct gaba_merge_tail_s));
			for(i = 0; i < tail->num_tail; i++) {
				if(tail->prem[i] != tail->p) {
					new_tail->tail[i] = fill_section_seq_p_bounded(
						this, tail->tail[i], a, b, tail->prem[i]);
				} else {
					new_tail->tail[i] = tail->tail[i];
				}
			}

			/* pが揃ったかチェック */
			if(p_is_aligned) {
				return(merge(this, tail));
			}
			#endif
		}
		#if 0
		/* merge-tail内のsectionとmaxの情報を更新する */
		tail->max = calc_max();
		tail->p = ...;
		#endif
	} else {
		/* tail is not merge_tail */
		return(_fill(fill_section_seq_bounded(this, tail, a, b)));
	}

	/* never reach here */
	return(NULL);
}

/**
 * @fn trace_search_max_block
 *
 * @brief detect a block which gives the maximum score in the fragment.
 *
 * @detail this function overwrites the traceback information in the dp context.
 */
static _force_inline
vec_t trace_search_max_block(
	struct gaba_dp_context_s *this,
	struct gaba_joint_tail_s const *tail)
{
	struct gaba_block_s *blk = _last_block(tail);

	debug("p(%d), psum(%lld), ssum(%d), offset(%lld)",
		tail->p, tail->psum, tail->ssum, blk->offset);

	/* search max section */
	int64_t offset = blk->offset;
	vec_t max = _load(&blk->sd.max);
	_print(max);

	/* determine q-coordinate */
	uint32_t mask_max = ((union gaba_mask_u){
		.mask = _mask_v32i16(_eq_v32i16(
			_set_v32i16(tail->max - offset),
			_add_v32i16(_load_v32i16(tail->v), _cvt_v32i8_v32i16(max))))
	}).all;
	debug("mask_max(%x)", mask_max);
	_print_v32i16(_set_v32i16(tail->max - offset));
	_print_v32i16(_add_v32i16(_load_v32i16(tail->v), _cvt_v32i8_v32i16(max)));

	/* iterate towards the head */
	uint32_t mask_update = mask_max;
	for(int32_t b = (tail->p - 1) / BLK; b >= 0; b--, blk--) {
		debug("b(%d), blk(%p)", b, blk);

		/* load the previous max vector and offset */
		vec_t prev_max = _load(&(blk - 1)->sd.max);
		int64_t prev_offset = (blk - 1)->offset;
		_print_v32i16(_add_v32i16(_add_v32i16(_load_v32i16(tail->v), _cvt_v32i8_v32i16(prev_max)), _set_v32i16(prev_offset)));

		/* adjust offset */
		// prev_max = _add(prev_max, _set(prev_offset - offset));
		max = _add(max, _set(offset - prev_offset));
		_print(max);
		_print(prev_max);

		/* take mask */
		uint32_t prev_mask_update = mask_update & ((union gaba_mask_u){
			.mask = _mask(_eq(prev_max, max))
		}).all;
		debug("mask_update(%x)", prev_mask_update);
		if(prev_mask_update == 0) {
			/* max update occured at current block */
			debug("blk(%p), len(%d), p(%d)", blk, MIN2(tail->p - b * BLK, BLK), b * BLK);
			
			/* save results to writer_work */
			this->ll.tail = tail;
			this->ll.blk = blk;
			this->ll.len = MIN2(tail->p - b * BLK, BLK);
			this->ll.p = b * BLK;
			this->ll.psum = tail->psum - tail->p + b * BLK;
			this->ll.mask_max = mask_update;
			return(max);
		}

		/* windback a block */
		max = prev_max;
		offset = prev_offset;
		mask_update = prev_mask_update;
	}
	/* save results */
	this->ll.tail = tail;
	this->ll.blk = blk;
	this->ll.len = 0;
	this->ll.p = 0;
	this->ll.mask_max = 0;
	return(max);
}

/**
 * @fn trace_calc_coordinates
 */
static _force_inline
void trace_calc_coordinates(
	struct gaba_dp_context_s *this,
	struct gaba_block_s const *blk,
	int64_t mask_idx,
	uint32_t mask_update)
{
	/* i \in [0 .. BLK) */
	/* adjust global coordinates with local coordinate */
	this->ll.p += mask_idx;
	this->ll.psum += mask_idx;
	int64_t q = this->ll.q = tzcnt(mask_update);

	debug("mask_idx(%lld), mask_update(%u)", mask_idx, mask_update);

	/* calc ridx */
	int64_t filled_count = mask_idx + 1;
	int32_t bcnt = _dir_bcnt(_dir_load(blk, mask_idx));
	int32_t acnt = filled_count - bcnt;
	v2i32_t ridx = _add_v2i32(
		_load_v2i32(&(blk - 1)->aridx),
		_seta_v2i32((BLK - 1 - q) - bcnt, q - acnt));

	debug("acnt(%d), bcnt(%d)", acnt, bcnt);
	_print_v2i32(_load_v2i32(&(blk - 1)->aridx));
	_print_v2i32(_seta_v2i32((BLK - 1 - q) - bcnt, q - acnt));
	_print_v2i32(ridx);

	/* save ridx */
	_store_v2i32(&this->ll.aidx, _sub_v2i32(_zero_v2i32(), ridx));

	return;
}

/**
 * @fn trace_search_max_pos
 *
 * @brief detect the local coordinate of the max score
 *
 * @detail block must be detected with trace_search_max_block before calling this.
 */
static _force_inline
void trace_search_max_pos(
	struct gaba_dp_context_s *this,
	vec_t compd_max)
{
	/* load coordinates, mask and pointers */
	struct gaba_joint_tail_s const *tail = this->ll.tail;
	struct gaba_block_s *blk = (struct gaba_block_s *)this->ll.blk;
	int64_t len = this->ll.len;
	uint32_t mask_max = this->ll.mask_max;
	debug("mask_max(%x)", mask_max);

	/* recalculate block */
	union gaba_mask_u mask_max_arr[BLK];
	union gaba_mask_u *mask_max_ptr = mask_max_arr;

	fill_restore_fetch(this, blk);
	/* vectors on registers inside this block */ {

		#define _fill_block_leaf(_mask_ptr) { \
			_dir_fetch(dir); \
			if(_dir_is_right(dir)) { \
				_fill_right_update_ptr(); \
				_fill_right(); \
			} else { \
				_fill_down_update_ptr(); \
				_fill_down(); \
			} \
			(_mask_ptr)++->mask = _mask(_eq(max, delta)); \
			debug("mask(%x)", ((union gaba_mask_u){ .mask = _mask(_eq(max, delta)) }).all); \
		}

		/* load contexts and overwrite max vector */
		_fill_load_contexts(blk);
		max = compd_max;		/* overwrite with compensated max vector */
		(void)offset;			/* to avoid warning */

		for(int64_t i = 0; i < len; i++) {
			_fill_block_leaf(mask_max_ptr);
			_print_v32i16(_add_v32i16(_add_v32i16(_load_v32i16(tail->v), _cvt_v32i8_v32i16(max)), _set_v32i16(offset)));
		}
	}

	/* determine max pos */
	// for(int64_t i = len-1; i >= 0; i--) {
	for(int64_t i = 0; i < len; i++) {
		uint32_t mask_update = mask_max_arr[i].all & mask_max;
		if(mask_update != 0) {
			debug("i(%lld), p(%lld), q(%d)",
				i, this->ll.p + i, tzcnt(mask_update) - BW/2);

			trace_calc_coordinates(this, blk, i, mask_update);
			return;
		}
	}

	/* not found in the block */
	debug("not found in the block (may be a bug)");
	v32i16_t md = _load_v32i16(tail->v);
	v32i16_t max = _cvt_v32i8_v32i16(_load(&(blk - 1)->sd.max));
	md = _add_v32i16(md, max);
	uint32_t mask = ((union gaba_mask_u){
		.mask = _mask_v32i16(_eq_v32i16(md, _set_v32i16(_hmax_v32i16(md))))
	}).all;
	debug("p(%d), q(%d)", this->ll.p - 1, tzcnt(mask & mask_max) - BW/2);
	
	/* save results to writer_work */
	trace_calc_coordinates(this, blk, 0, mask & mask_max);
	return;
}

/**
 * @fn trace_load_section_a, trace_load_section_b
 */
static _force_inline
void trace_load_section_a(
	struct gaba_dp_context_s *this)
{
	debug("load section a");
	/* load tail pointer (must be inited with leaf tail) */
	struct gaba_joint_tail_s const *tail = this->ll.atail;
	int32_t idx = this->ll.aidx;
	int32_t len = tail->a.len;

	while(idx <= 0) {
		if(tail->tail == NULL) {
			log("NULL detected in load_section_a: "
				"atail(%p, %d), btail(%p, %d), tail(%p, %d), "
				"p(%d), psum(%lld), q(%d), "
				"len(%d, %d), idx(%d, %d), sidx(%d, %d)",
				tail, tail->ssum,
				this->ll.btail, this->ll.btail->ssum,
				this->ll.tail, this->ll.tail->ssum,
				this->ll.p, this->ll.psum, this->ll.q,
				this->ll.blen, this->ll.alen,
				this->ll.bidx, this->ll.aidx,
				this->ll.bsidx, this->ll.asidx);
		}
		for(tail = tail->tail; tail->apos != 0; tail = tail->tail) {
			if(tail->tail == NULL) {
				log("NULL detected in load_section_a: "
					"atail(%p, %d), btail(%p, %d), tail(%p, %d), "
					"p(%d), psum(%lld), q(%d), "
					"len(%d, %d), idx(%d, %d), sidx(%d, %d)",
					tail, tail->ssum,
					this->ll.btail, this->ll.btail->ssum,
					this->ll.tail, this->ll.tail->ssum,
					this->ll.p, this->ll.psum, this->ll.q,
					this->ll.blen, this->ll.alen,
					this->ll.bidx, this->ll.aidx,
					this->ll.bsidx, this->ll.asidx);
			}
		}

		/* update ridx and len */
		idx += len;
		len = tail->a.len;
	}

	/* store section info */
	v2i64_t sec = _load_v2i64(&tail->a);
	_store_v2i64(&this->ll.asec, sec);
	this->ll.atail = tail;
	this->ll.alen = len;
	this->ll.aidx = idx;
	this->ll.asidx = idx;
	return;
}
static _force_inline
void trace_load_section_b(
	struct gaba_dp_context_s *this)
{
	debug("load section b");
	/* load tail pointer (must be inited with leaf tail) */
	struct gaba_joint_tail_s const *tail = this->ll.btail;
	int32_t idx = this->ll.bidx;
	int32_t len = tail->b.len;

	// while(ridx >= len) {
	while(idx <= 0) {
		if(tail->tail == NULL) {
			log("NULL detected in load_section_b: "
				"atail(%p, %d), btail(%p, %d), tail(%p, %d), "
				"p(%d), psum(%lld), q(%d), "
				"len(%d, %d), idx(%d, %d), sidx(%d, %d)",
				this->ll.atail, this->ll.atail->ssum,
				tail, tail->ssum,
				this->ll.tail, this->ll.tail->ssum,
				this->ll.p, this->ll.psum, this->ll.q,
				this->ll.blen, this->ll.alen,
				this->ll.bidx, this->ll.aidx,
				this->ll.bsidx, this->ll.asidx);
		}
		for(tail = tail->tail; tail->bpos != 0; tail = tail->tail) {
			if(tail->tail == NULL) {
				log("NULL detected in load_section_b: "
					"atail(%p, %d), btail(%p, %d), tail(%p, %d), "
					"p(%d), psum(%lld), q(%d), "
					"len(%d, %d), idx(%d, %d), sidx(%d, %d)",
					this->ll.atail, this->ll.atail->ssum,
					tail, tail->ssum,
					this->ll.tail, this->ll.tail->ssum,
					this->ll.p, this->ll.psum, this->ll.q,
					this->ll.blen, this->ll.alen,
					this->ll.bidx, this->ll.aidx,
					this->ll.bsidx, this->ll.asidx);
			}
		}

		/* update ridx and len */
		idx += len;
		len = tail->b.len;
	}

	/* store section info */
	v2i64_t sec = _load_v2i64(&tail->b);
	_store_v2i64(&this->ll.bsec, sec);
	this->ll.btail = tail;
	this->ll.blen = len;
	this->ll.bidx = idx;
	this->ll.bsidx = idx;
	return;
}

/**
 * @macro _trace_load_context, _trace_forward_load_context
 */
#define _trace_load_context(t) \
	v2i32_t idx = _load_v2i32(&(t)->ll.aidx); \
	struct gaba_block_s const *blk = (t)->ll.blk; \
	int64_t p = (t)->ll.p; \
	int64_t q = (t)->ll.q; \
	int64_t diff_q = 0; \
	int64_t prev_c = 0;
#define _trace_forward_load_context(t) \
	_trace_load_context(t); \
	uint32_t *path = (t)->ll.fw_path; \
	int64_t rem = (t)->ll.fw_rem;
#define _trace_reverse_load_context(t) \
	_trace_load_context(t); \
	uint32_t *path = (t)->ll.rv_path; \
	int64_t rem = (t)->ll.rv_rem; \
	/* reverse q-coordinate in the reverse trace */ \
	debug("reverse q-coordinate q(%lld), rq(%lld)", q, (BW - 1) - q); \
	q = (BW - 1) - q;

/**
 * @macro _trace_load_block
 */
#define _trace_load_block(_local_idx) \
	union gaba_dir_u dir = _dir_load(blk, (_local_idx)); \
	union gaba_mask_pair_u const *mask_ptr = &blk->mask[(_local_idx)]; \
	uint64_t path_array = 0; \
	/* working registers need not be initialized */ \
	v2i32_t next_idx; \
	int64_t next_diff_q; \
	uint64_t next_path_array, next_prev_c;

/**
 * @macro _trace_forward_determine_direction
 */
#define _trace_forward_determine_direction(_mask) ({ \
	uint64_t _mask_h = (_mask)>>q; \
	uint64_t _mask_v = _mask_h>>BW; \
	int64_t _is_down = (0x01 & _mask_v) | prev_c; \
	next_path_array = (path_array<<1) | _is_down; \
	next_diff_q = _dir_is_down(dir) - _is_down; \
	next_prev_c = 0x01 & ~(prev_c | _mask_h | _mask_v); \
	debug("c(%lld), prev_c(%llu), p(%lld), psum(%lld), q(%lld), diff_q(%lld), down(%d), mask(%llx), path_array(%llx)", \
		_is_down, prev_c, p, this->ll.psum - (this->ll.p - p), q, next_diff_q, _dir_is_down(dir), _mask, path_array); \
	_is_down; \
})
#define _trace_reverse_determine_direction(_mask) ({ \
	uint64_t _mask_h = (_mask)<<q; \
	uint64_t _mask_v = _mask_h>>BW; \
	int64_t _is_right = (0x80000000 & _mask_h) | prev_c; \
	next_path_array = (path_array>>1) | _is_right; \
	next_diff_q = (int64_t)!_is_right - _dir_is_down(dir); \
	next_prev_c = 0x80000000 & ~(prev_c | _mask_h | _mask_v); \
	debug("c(%lld), prev_c(%llu), p(%lld), psum(%lld), q(%lld), diff_q(%lld), down(%d), mask(%llx), path_array(%llx)", \
		_is_right>>31, prev_c>>31, p, this->ll.psum - (this->ll.p - p), q, next_diff_q, _dir_is_down(dir), _mask, path_array); \
	_is_right; \
})

/**
 * @macro _trace_update_index
 */
#define _trace_update_index() { \
	q += next_diff_q; \
	diff_q = next_diff_q; \
	path_array = next_path_array; \
	prev_c = next_prev_c; \
	mask_ptr--; \
	_dir_windback(dir); \
}

/**
 * @macro _trace_forward_bulk_body
 */
#define _trace_forward_bulk_body() { \
	_trace_forward_determine_direction(mask_ptr->all); \
	_trace_update_index(); \
}
#define _trace_reverse_bulk_body() { \
	_trace_reverse_determine_direction(mask_ptr->all); \
	_trace_update_index(); \
}

/**
 * @macro _trace_forward_cap_body
 */
#define _trace_forward_cap_body() { \
	int64_t _is_down = _trace_forward_determine_direction(mask_ptr->all); \
	/* calculate the next ridx */ \
	v2i32_t const idx_down = _seta_v2i32(1, 0); \
	v2i32_t const idx_right = _seta_v2i32(0, 1); \
	next_idx = _sub_v2i32(idx, _is_down ? idx_down : idx_right); \
	_print_v2i32(next_idx); \
}
#define _trace_reverse_cap_body() { \
	int64_t _is_right = _trace_reverse_determine_direction(mask_ptr->all); \
	/* calculate the next ridx */ \
	v2i32_t const idx_down = _seta_v2i32(1, 0); \
	v2i32_t const idx_right = _seta_v2i32(0, 1); \
	next_idx = _sub_v2i32(idx, _is_right ? idx_right : idx_down); \
	_print_v2i32(next_idx); \
}

/**
 * @macro _trace_cap_update_index
 */
#define _trace_cap_update_index() { \
	_trace_update_index(); \
 	idx = next_idx; \
	p--; \
}

/**
 * @macro _trace_test_cap_loop
 *
 * @brief returns non-zero if ridx >= len
 */
#define _trace_test_cap_loop() ( \
	/* next_ridx must be precalculated */ \
	_mask_v2i32(_lt_v2i32(next_idx, _zero_v2i32())) \
)
#define _trace_test_bulk_loop() ( \
	_mask_v2i32(_lt_v2i32(idx, _set_v2i32(BW))) \
)

/** 
 * @macro _trace_bulk_update_index
 */
#define _trace_bulk_update_index() { \
	int32_t bcnt = popcnt(path_array); \
	/*idx = _sub_v2i32(idx, _sub_v2i32(*/ \
		/*_load_v2i32(&(blk - 1)->aridx),*/ \
		/*_load_v2i32(&blk->aridx)));*/ \
	idx = _sub_v2i32(idx, _seta_v2i32(bcnt, BLK - bcnt)); \
	debug("bulk update index p(%lld)", p); \
	_print_v2i32(idx); \
	p -= BLK; \
}

/**
 * @macro _trace_forward_update_path, _trace_backward_update_path
 */
#define _trace_forward_compensate_remainder(_traced_count) ({ \
	/* decrement cnt when invasion of boundary on seq b with diagonal path occurred */ \
	int64_t _cnt = (_traced_count) - prev_c; \
	path_array >>= prev_c; \
	p += prev_c; \
	q -= (prev_c == 0) ? 0 : diff_q; \
	idx = _add_v2i32(idx, _seta_v2i32(0, prev_c)); \
	debug("cnt(%lld), traced_count(%lld), prev_c(%lld), path_array(%llx), p(%lld)", \
		_cnt, _traced_count, prev_c, path_array, p); \
	_cnt; \
})
#define _trace_reverse_compensate_remainder(_traced_count) ({ \
	/* decrement cnt when invasion of boundary on seq b with diagonal path occurred */ \
	prev_c >>= 31; \
	int64_t _cnt = (_traced_count) - prev_c; \
	path_array <<= prev_c; \
	p += prev_c; \
	q -= (prev_c == 0) ? 0 : diff_q; \
	idx = _add_v2i32(idx, _seta_v2i32(prev_c, 0)); \
	debug("cnt(%lld), traced_count(%lld), prev_c(%lld), path_array(%llx), p(%lld)", \
		_cnt, _traced_count, prev_c, path_array, p); \
	_cnt; \
})
#define _trace_forward_update_path(_traced_count) { \
	/* adjust path_array */ \
	path_array = path_array<<(BLK - (_traced_count)); \
	/* load the previous array */ \
	uint64_t prev_array = path[0]; \
	/* write back array */ \
	path[-1] = (uint32_t)(path_array<<(BLK - rem)); \
	path[0] = (uint32_t)(prev_array | (path_array>>rem)); \
	debug("prev_array(%llx), path_array(%llx), path[-1](%x), path[0](%x), blk(%p)", \
		prev_array, path_array, path[-1], path[0], blk); \
	/* update path and remainder */ \
	path -= (rem + (_traced_count)) / BLK; \
	debug("path_adv(%d)", ((rem + (_traced_count)) & BLK) != 0 ? 1 : 0); \
	rem = (rem + (_traced_count)) & (BLK - 1); \
	debug("i(%lld), rem(%lld)", (int64_t)(_traced_count), rem); \
}
#define _trace_reverse_update_path(_traced_count) { \
	/* adjust path_array */ \
	path_array = (uint64_t)(~((uint32_t)path_array))>>(BLK - (_traced_count)); \
	/* load previous array */ \
	uint64_t prev_array = path[0]; \
	/* write back array */ \
	path[0] = (uint32_t)(prev_array | (path_array<<rem)); \
	path[1] = (uint32_t)(path_array>>(BLK - rem)); \
	debug("prev_array(%llx), path_array(%llx), path[0](%x), path[1](%x), blk(%p)", \
		prev_array, path_array, path[0], path[1], blk); \
	/* update path and remainder */ \
	path += (rem + (_traced_count)) / BLK; \
	debug("path_adv(%d)", ((rem + (_traced_count)) & BLK) != 0 ? 1 : 0); \
	rem = (rem + (_traced_count)) & (BLK - 1); \
	debug("i(%lld), rem(%lld)", (int64_t)(_traced_count), rem); \
}

/**
 * @macro _trace_windback_block
 */
#define _trace_windback_block() { \
	/* load the previous block */ \
	blk--; \
}

/**
 * @macro _trace_forward_save_context
 */
#define _trace_save_context(t) { \
	(t)->ll.blk = blk; \
	_store_v2i32(&(t)->ll.aidx, idx); \
	(t)->ll.psum -= (t)->ll.p - p; \
	(t)->ll.p = p; \
	(t)->ll.q = q; \
	debug("p(%lld), psum(%lld), q(%llu)", p, (t)->ll.psum, q); \
}
#define _trace_forward_save_context(t) { \
	(t)->ll.fw_path = path; \
	(t)->ll.fw_rem = rem; \
	_trace_save_context(t); \
}
#define _trace_reverse_save_context(t) { \
	/* reverse q-coordinate in the reverse trace */ \
	debug("unreverse q-coordinate rq(%lld), q(%lld)", q, (BW - 1) - q); \
	q = (BW - 1) - q; \
	(t)->ll.rv_path = path; \
	(t)->ll.rv_rem = rem; \
	_trace_save_context(t); \
}

/**
 * @macro _trace_reload_tail
 */
#define _trace_reload_tail(t) { \
	debug("tail(%p), next tail(%p), p(%d), psum(%lld), ssum(%d)", \
		(t)->ll.tail, (t)->ll.tail->tail, (t)->ll.tail->tail->p, \
		(t)->ll.tail->tail->psum, (t)->ll.tail->tail->ssum); \
	struct gaba_joint_tail_s const *tail = (t)->ll.tail = (t)->ll.tail->tail; \
	blk = _last_block(tail); \
	(t)->ll.psum -= (t)->ll.p; \
	p += ((t)->ll.p = tail->p); \
	debug("updated psum(%lld), ll.p(%d), p(%lld)", (t)->ll.psum, (t)->ll.p, p); \
}

/**
 * @fn trace_forward_trace
 *
 * @brief traceback path until section invasion occurs
 */
static
void trace_forward_trace(
	struct gaba_dp_context_s *this)
{
	/* load context onto registers */
	_trace_forward_load_context(this);
	debug("start traceback loop p(%lld), q(%llu)", p, q);

	while(1) {
		/* cap trace at the head */
		if(p > 0 && (p + 1) % BLK != 0) {
			int64_t loop_count = (p + 1) % BLK;		/* loop_count \in (1 .. BLK), 0 is invalid */

			/* load direction array and mask pointer */
			_trace_load_block(loop_count - 1);

			/* cap trace loop */
			for(int64_t i = 0; i < loop_count; i++) {
				_trace_forward_cap_body();

				/* check if the section invasion occurred */
				if(_trace_test_cap_loop() != 0) {
					debug("cap test failed, i(%lld)", i);
					i = _trace_forward_compensate_remainder(i);
					_trace_forward_update_path(i);
					_trace_forward_save_context(this);
					return;
				}

				_trace_cap_update_index();
			}

			/* update rem, path, and block */
			_trace_forward_update_path(loop_count);
			_trace_windback_block();
		}

		/* bulk trace */
		while(p > 0 && _trace_test_bulk_loop() == 0) {
			/* load direction array and mask pointer */
			_trace_load_block(BLK - 1);
			(void)next_idx;		/* to avoid warning */

			/* bulk trace loop */
			for(int64_t i = 0; i < BLK; i++) {
				_trace_forward_bulk_body();
			}

			/* update path and block */
			_trace_bulk_update_index();
			_trace_forward_update_path(BLK);
			_trace_windback_block();

			debug("bulk loop end p(%lld), q(%llu)", p, q);
		}

		/* bulk trace test failed, continue to cap trace */
		while(p > 0) {
			/* cap loop */
			_trace_load_block(BLK - 1);

			/* cap trace loop */
			for(int64_t i = 0; i < BLK; i++) {
				_trace_forward_cap_body();

				/* check if the section invasion occurred */
				if(_trace_test_cap_loop() != 0) {
					debug("cap test failed, i(%lld)", i);
					i = _trace_forward_compensate_remainder(i);
					_trace_forward_update_path(i);
					_trace_forward_save_context(this);
					return;		/* exit is here */
				}

				_trace_cap_update_index();
			}
			debug("bulk loop end p(%lld), q(%llu)", p, q);

			/* update path and block */
			_trace_forward_update_path(BLK);
			_trace_windback_block();
		}

		/* check psum termination */
		if(this->ll.psum < this->ll.p - p) {
			_trace_forward_save_context(this);
			return;
		}

		/* reload tail pointer */
		_trace_reload_tail(this);
	}
	return;
}

/**
 * @fn trace_reverse_trace
 *
 * @brief traceback path until section invasion occurs
 */
static
void trace_reverse_trace(
	struct gaba_dp_context_s *this)
{
	/* load context onto registers */
	_trace_reverse_load_context(this);
	debug("start traceback loop p(%lld), q(%llu)", p, q);

	while(1) {
		/* cap trace at the head */ {
			int64_t loop_count = (p + 1) % BLK;		/* loop_count \in (1 .. BLK), 0 is invalid */

			/* load direction array and mask pointer */
			_trace_load_block(loop_count - 1);

			/* cap trace loop */
			for(int64_t i = 0; i < loop_count; i++) {
				_trace_reverse_cap_body();

				/* check if the section invasion occurred */
				if(_trace_test_cap_loop() != 0) {
					debug("cap test failed, i(%lld)", i);
					i = _trace_reverse_compensate_remainder(i);
					_trace_reverse_update_path(i);
					_trace_reverse_save_context(this);
					return;
				}

				_trace_cap_update_index();
			}

			/* update rem, path, and block */
			_trace_reverse_update_path(loop_count);
			_trace_windback_block();
		}

		/* bulk trace */
		while(p > 0 && _trace_test_bulk_loop() == 0) {
			/* load direction array and mask pointer */
			_trace_load_block(BLK - 1);
			(void)next_idx;		/* to avoid warning */

			/* bulk trace loop */
			_trace_bulk_update_index();
			for(int64_t i = 0; i < BLK; i++) {
				_trace_reverse_bulk_body();
			}
			debug("bulk loop end p(%lld), q(%llu)", p, q);

			/* update path and block */
			_trace_reverse_update_path(BLK);
			_trace_windback_block();
		}

		/* bulk trace test failed, continue to cap trace */
		while(p > 0) {
			/* cap loop */
			_trace_load_block(BLK - 1);

			/* cap trace loop */
			for(int64_t i = 0; i < BLK; i++) {
				_trace_reverse_cap_body();

				/* check if the section invasion occurred */
				if(_trace_test_cap_loop() != 0) {
					debug("cap test failed, i(%lld)", i);
					i = _trace_reverse_compensate_remainder(i);
					_trace_reverse_update_path(i);
					_trace_reverse_save_context(this);
					return;		/* exit is here */
				}

				_trace_cap_update_index();
			}
			debug("bulk loop end p(%lld), q(%llu)", p, q);

			/* update path and block */
			_trace_reverse_update_path(BLK);
			_trace_windback_block();
		}

		/* check psum termination */
		if(this->ll.psum < this->ll.p - p) {
			_trace_reverse_save_context(this);
			return;
		}

		/* reload tail pointer */
		_trace_reload_tail(this);
	}
	return;
}

/**
 * @fn trace_forward_push
 */
static
void trace_forward_push(
	struct gaba_dp_context_s *this)
{
	/* push current section info */
	_store_v2i64(&this->ll.fw_sec->a, _load_v2i64(&this->ll.asec));
	_store_v2i64(&this->ll.fw_sec->b, _load_v2i64(&this->ll.bsec));

	debug("push current section info");
	_print_v2i64(_load_v2i64(&this->ll.asec));
	_print_v2i64(_load_v2i64(&this->ll.bsec));

	/* store segment info */
	v2i32_t idx = _load_v2i32(&this->ll.aidx);
	v2i32_t sidx = _load_v2i32(&this->ll.asidx);
	_store_v2i32(&this->ll.fw_sec->apos, idx);
	_store_v2i32(&this->ll.fw_sec->alen, _sub_v2i32(sidx, idx));

	/* update rsidx */
	// _store_v2i32(&this->ll.asridx, ridx);
	_store_v2i32(&this->ll.asidx, idx);

	debug("push segment info");
	_print_v2i32(idx);
	_print_v2i32(_sub_v2i32(sidx, idx));

	/* windback pointer */
	this->ll.fw_sec--;
	this->ll.fw_scnt++;
	return;
}

/**
 * @fn trace_reverse_push
 */
static
void trace_reverse_push(
	struct gaba_dp_context_s *this)
{
	/* push current (revcomped) section info */
	this->ll.rv_sec->a = (struct gaba_section_s){
		.id = -this->ll.asec.id,
		.len = this->ll.asec.len,
		.base = 2 * this->rr.p.alen - this->ll.asec.base - this->ll.asec.len
	};
	this->ll.rv_sec->b = (struct gaba_section_s){
		.id = -this->ll.bsec.id,
		.len = this->ll.bsec.len,
		.base = 2 * this->rr.p.blen - this->ll.bsec.base - this->ll.bsec.len
	};

	debug("push current section info");
	_print_v2i64(_load_v2i64(&this->ll.rv_sec->a));
	_print_v2i64(_load_v2i64(&this->ll.rv_sec->b));

	/* store segment info */
	v2i32_t len = _load_v2i32(&this->ll.alen);
	v2i32_t idx = _load_v2i32(&this->ll.aidx);
	v2i32_t sidx = _load_v2i32(&this->ll.asidx);

	_store_v2i32(&this->ll.rv_sec->apos, _sub_v2i32(len, sidx));
	_store_v2i32(&this->ll.rv_sec->alen, _sub_v2i32(sidx, idx));

	/* update rsidx */
	_store_v2i32(&this->ll.asidx, idx);

	debug("push segment info");
	_print_v2i32(len);
	_print_v2i32(_sub_v2i32(len, sidx));
	_print_v2i32(_sub_v2i32(sidx, idx));

	/* windback pointer */
	this->ll.rv_sec++;
	this->ll.rv_scnt++;
	return;
}

/**
 * @fn trace_forward and trace_reverse
 */
#define TRACE_FORWARD 		( 0 )
#define TRACE_REVERSE 		( 1 )
static _force_inline
void trace_generate_path(
	struct gaba_dp_context_s *this,
	struct gaba_joint_tail_s const *tail,
	uint64_t dir)
{
	debug("trace_init_start_pos tail(%p), p(%d), psum(%lld), ssum(%d)",
		tail, tail->p, tail->psum, tail->ssum);

	if(tail->psum < 0) {
		debug("valid block not found, psum(%lld)", tail->psum);
		return;
	}

	/* initialize sections and pointers */
	_store_v2i64(&this->ll.asec, _set_v2i64(-1));
	_store_v2i64(&this->ll.bsec, _set_v2i64(-1));
	this->ll.tail = tail;
	this->ll.atail = tail;
	this->ll.btail = tail;

	/* set initial section lengths */
	v2i32_t const z = _zero_v2i32();
	_store_v2i32(&this->ll.alen, z);

	/* initialize function pointers */
	void (*body)(struct gaba_dp_context_s *this) = (
		(dir == TRACE_FORWARD) ? trace_forward_trace : trace_reverse_trace
	);
	void (*push)(struct gaba_dp_context_s *this) = (
		(dir == TRACE_FORWARD) ? trace_forward_push : trace_reverse_push
	);

	/* search max block */
	// trace_search_max_block(this, tail);

	/* determine pos in the block */
	trace_search_max_pos(this, trace_search_max_block(this, tail));

	/* until the pointer reaches the root of the matrix */
	while(this->ll.psum >= 0) {
		/* update section info */
		if(this->ll.aidx <= 0) {
			trace_load_section_a(this);
		}
		if(this->ll.bidx <= 0) {
			trace_load_section_b(this);
		}

		/* fragment trace */
		body(this);
		debug("p(%d), psum(%lld), q(%d)", this->ll.p, this->ll.psum, this->ll.q);

		/* push section info to section array */
		push(this);
	}
	return;
}

/**
 * @fn trace_concatenate_path
 */
static _force_inline
struct gaba_result_s *trace_concatenate_path(
	struct gaba_dp_context_s *this,
	uint32_t *fw_path_base,
	uint32_t *rv_path_base,
	struct gaba_path_section_s *fw_sec_base,
	struct gaba_path_section_s *rv_sec_base,
	int64_t score_sum)
{
	/* fixme: current implementation handle forward path only */
	struct gaba_result_s *res = (struct gaba_result_s *)this->ll.ptr;

	debug("fw_path_base(%p), rv_path_base(%p), fw_sec_base(%p), rv_sec_base(%p)",
		fw_path_base, rv_path_base, fw_sec_base, rv_sec_base);
	debug("fw_path(%p), rv_path(%p), fw_sec(%p), rv_sec(%p)",
		this->ll.fw_path, this->ll.rv_path, this->ll.fw_sec, this->ll.rv_sec);
	debug("fw_scnt(%d), rv_scnt(%d)", this->ll.fw_scnt, this->ll.rv_scnt);
	debug("fw_rem(%d), rv_rem(%d)", this->ll.fw_rem, this->ll.rv_rem);


	/* push forward path and update rem */
	uint64_t fw_rem = this->ll.fw_rem;
	uint64_t rv_rem = this->ll.rv_rem;
	uint32_t *fw_path = this->ll.fw_path;
	uint32_t *rv_path = this->ll.rv_path;

	/* concatenate the heads */
	uint64_t prev_array = *fw_path;
	uint64_t path_array = *rv_path--<<(BLK - rv_rem);

	debug("fw_path_array(%llx), rv_path_array(%llx)", prev_array, path_array);

	fw_path[0] = prev_array | (path_array>>fw_rem);
	fw_path[-1] = path_array<<(BLK - fw_rem);
	debug("path_array(%x), prev_array(%x)", fw_path[0], fw_path[-1]);

	debug("dec fw_path(%d), fw_rem(%llu), rv_rem(%llu), rem(%llu)",
		(((fw_rem + rv_rem) & BLK) != 0) ? 1 : 0,
		fw_rem, rv_rem, (fw_rem + rv_rem) & (BLK - 1));
	fw_path -= (((fw_rem + rv_rem) & BLK) != 0) ? 1 : 0;
	fw_rem = (fw_rem + rv_rem) & (BLK - 1);

	if(rv_path >= rv_path_base) {
		/* load array */
		prev_array = fw_path[0];

		while(rv_path >= rv_path_base) {
			path_array = *rv_path--;
			*fw_path-- = prev_array | (path_array>>fw_rem);
			prev_array = path_array<<(BLK - fw_rem);
			debug("rv_path(%llx), path_array(%x), prev_array(%llx)", path_array, fw_path[1], prev_array);
		}

		/* store array */
		fw_path[0] = prev_array;
		// fw_path += fw_rem == 0;
	}

	/* create path info container */
	struct gaba_path_s *path = (struct gaba_path_s *)(
		(uint8_t *)(fw_path + (fw_rem == 0)) - sizeof(struct gaba_path_s));

	/* calc path length */
	int64_t fw_path_block_len = fw_path_base - fw_path;
	int64_t path_len = 32 * fw_path_block_len + fw_rem;

	/* store path info */
	path->len = path_len;
	path->rem = fw_rem;
	res->path = path;

	/* push forward section and calc section length */
	int64_t sec_len = this->ll.fw_scnt + this->ll.rv_scnt;
	struct gaba_path_section_s *fw_sec = this->ll.fw_sec;
	struct gaba_path_section_s *rv_sec = this->ll.rv_sec;
	while(rv_sec > rv_sec_base) {
		*fw_sec-- = *--rv_sec;
		debug("copy reverse section");
	}
	res->sec = fw_sec + 1;
	res->slen = sec_len;

	/* store score */
	int64_t score_adj = 0;		/* score adjustment at the root is not implemented */
	res->score = score_sum + score_adj;

	debug("sec(%p), path(%p), path_array(%x, %x, %x, %x), rem(%llu), slen(%lld), plen(%lld)",
		fw_sec, fw_path, fw_path[0], fw_path[1], fw_path[2], fw_path[3], fw_rem, sec_len, path_len);
	return(res);
}

/**
 * @fn gaba_dp_trace
 */
struct gaba_result_s *gaba_dp_trace(
	struct gaba_dp_context_s *this,
	struct gaba_fill_s const *fw_tail,
	struct gaba_fill_s const *rv_tail,
	struct gaba_clip_params_s const *clip)
{
	/* substitute tail if NULL */
	fw_tail = (fw_tail == NULL) ? _fill(this->tail) : fw_tail;
	rv_tail = (rv_tail == NULL) ? _fill(this->tail) : rv_tail;

	/* clear working variables */
	this->ll.fw_rem = 0;
	this->ll.rv_rem = 0;
	this->ll.fw_scnt = 0;
	this->ll.rv_scnt = 0;

	/* calculate max total path length */
	uint64_t psum = roundup(_tail(fw_tail)->psum + BLK, 32)
				  + roundup(_tail(rv_tail)->psum + BLK, 32);
	uint64_t ssum = _tail(fw_tail)->ssum + _tail(rv_tail)->ssum;

	/* malloc trace working area */
	uint64_t path_len = roundup(psum / 32, 4);
	uint64_t sec_len = 2 * ssum;
	debug("psum(%lld), path_len(%llu), sec_len(%llu)", psum, path_len, sec_len);

	uint64_t path_size = sizeof(uint32_t) * path_len;
	uint64_t sec_size = sizeof(struct gaba_path_section_s) * sec_len;
	this->ll.ptr = (uint8_t *)gaba_dp_malloc(this,
		sizeof(struct gaba_result_s) + path_size + sec_size);

	/* set path pointers */
	uint32_t *path_base = (uint32_t *)(this->ll.ptr + sizeof(struct gaba_result_s));
	uint32_t *fw_path_base = this->ll.fw_path = path_base + path_len - 1;
	uint32_t *rv_path_base = this->ll.rv_path = path_base;

	/* clear path array */
	*this->ll.fw_path = 0;
	*this->ll.rv_path = 0;

	/* set sec pointers */
	struct gaba_path_section_s *sec_base = (struct gaba_path_section_s *)&path_base[path_len];
	struct gaba_path_section_s *fw_sec_base = this->ll.fw_sec = sec_base + sec_len - 1;
	struct gaba_path_section_s *rv_sec_base = this->ll.rv_sec = sec_base;

	/* forward trace */
	trace_generate_path(this, _tail(fw_tail), TRACE_FORWARD);

	/* reverse trace */
	trace_generate_path(this, _tail(rv_tail), TRACE_REVERSE);

	/* concatenate */
	return(trace_concatenate_path(this,
		fw_path_base, rv_path_base,
		fw_sec_base, rv_sec_base,
		fw_tail->max + rv_tail->max));
}

/**
 * @fn extract_max
 * @brief extract max value from 8-bit 16-cell vector
 */
static _force_inline
int8_t extract_max(int8_t const vector[][4])
{
	int8_t *v = (int8_t *)vector;
	int8_t max = -128;
	for(int i = 0; i < 16; i++) {
		max = (v[i] > max) ? v[i] : max;
	}
	return(max);
}

/**
 * @fn gaba_init_restore_default_params
 */
static
struct gaba_score_s const *default_score_matrix = GABA_SCORE_SIMPLE(1, 1, 1, 1);
static _force_inline
void gaba_init_restore_default_params(
	struct gaba_params_s *params)
{
	#define restore(_name, _default) { \
		params->_name = ((uint64_t)(params->_name) == 0) ? (_default) : (params->_name); \
	}
	restore(seq_a_format, 		GABA_ASCII);
	restore(seq_a_direction, 	GABA_FW_ONLY);
	restore(seq_b_format, 		GABA_ASCII);
	restore(seq_b_direction, 	GABA_FW_ONLY);
	restore(head_margin, 		0);
	restore(tail_margin, 		0);
	restore(xdrop, 				100);
	restore(score_matrix, 		default_score_matrix);
	return;
}

/**
 * @fn gaba_init_create_score_vector
 */
static _force_inline
struct gaba_score_vec_s gaba_init_create_score_vector(
	struct gaba_score_s const *score_matrix)
{
	int8_t *v = (int8_t *)score_matrix->score_sub;
	int8_t geh = -score_matrix->score_ge_a;
	int8_t gev = -score_matrix->score_ge_b;
	int8_t gih = -score_matrix->score_gi_a;
	int8_t giv = -score_matrix->score_gi_b;
	struct gaba_score_vec_s sc;
	for(int i = 0; i < 16; i++) {
		sc.sb[i] = v[i] - (geh + gih + gev + giv);
		sc.adjh[i] = -gih;
		sc.adjv[i] = -giv;
		sc.ofsh[i] = -(geh + gih);
		sc.ofsv[i] = gev + giv;
	}
	return(sc);
}

/**
 * @fn gaba_init_create_dir_dynamic
 */
static _force_inline
union gaba_dir_u gaba_init_create_dir_dynamic(
	struct gaba_score_s const *score_matrix)
{
	int8_t max = extract_max(score_matrix->score_sub);
	int8_t geh = -score_matrix->score_ge_a;
	int8_t gev = -score_matrix->score_ge_b;
	int8_t gih = -score_matrix->score_gi_a;
	int8_t giv = -score_matrix->score_gi_b;

	int16_t coef_a = -max + 2*geh;
	int16_t coef_b = -max + 2*gev;
	int16_t ofs_a = gih;
	int16_t ofs_b = giv;

	int64_t acc = (ofs_a + coef_a * BW/2) - (ofs_b + coef_b * (BW/2 - 1));
	return((union gaba_dir_u) {
		.dynamic = {
			.acc = acc,
			.array = 0x80000000	/* (0, 0) -> (0, 1) (down) */
		}
	});
}

/**
 * @fn gaba_init_create_small_delta
 */
static _force_inline
struct gaba_small_delta_s gaba_init_create_small_delta(
	struct gaba_score_s const *score_matrix)
{
	return((struct gaba_small_delta_s){
		.delta = {0},
		.max = {0}
	});
}

/**
 * @fn gaba_init_create_middle_delta
 */
static _force_inline
struct gaba_middle_delta_s gaba_init_create_middle_delta(
	struct gaba_score_s const *score_matrix)
{
	int8_t max = extract_max(score_matrix->score_sub);
	int8_t geh = -score_matrix->score_ge_a;
	int8_t gev = -score_matrix->score_ge_b;
	int8_t gih = -score_matrix->score_gi_a;
	int8_t giv = -score_matrix->score_gi_b;

	int16_t coef_a = -max + 2*geh;
	int16_t coef_b = -max + 2*gev;
	int16_t ofs_a = gih;
	int16_t ofs_b = giv;

	struct gaba_middle_delta_s md;
	for(int i = 0; i < BW/2; i++) {
		md.delta[i] = ofs_a + coef_a * (BW/2 - i);
		md.delta[BW/2 + i] = ofs_b + coef_b * i;
	}
	md.delta[BW/2] = 0;
	return(md);
}

/**
 * @fn gaba_init_create_diff_vectors
 *
 * @detail
 * dH[i, j] = S[i, j] - S[i - 1, j]
 * dV[i, j] = S[i, j] - S[i, j - 1]
 * dE[i, j] = E[i, j] - S[i, j]
 * dF[i, j] = F[i, j] - S[i, j]
 */
static _force_inline
struct gaba_diff_vec_s gaba_init_create_diff_vectors(
	struct gaba_score_s const *score_matrix)
{
	int8_t max = extract_max(score_matrix->score_sub);
	int8_t geh = -score_matrix->score_ge_a;
	int8_t gev = -score_matrix->score_ge_b;
	int8_t gih = -score_matrix->score_gi_a;
	int8_t giv = -score_matrix->score_gi_b;

	int8_t ofs_dh = -(geh + gih);
	int8_t ofs_dv = -(gev + giv);
	int8_t ofs_de = -gih;
	int8_t ofs_df = -giv;
	
	int8_t drop_dh = geh + ofs_dh;
	int8_t raise_dh = max - gev + ofs_dh;
	int8_t drop_dv = gev + ofs_dv;
	int8_t raise_dv = max - geh + ofs_dv;
	int8_t drop_de = gih + ofs_de;
	int8_t raise_de = ofs_de;
	int8_t drop_df = giv + ofs_df;
	int8_t raise_df = ofs_df;

	int8_t dh[BW] __attribute__(( aligned(16) ));
	int8_t dv[BW] __attribute__(( aligned(16) ));
	int8_t de[BW] __attribute__(( aligned(16) ));
	int8_t df[BW] __attribute__(( aligned(16) ));

	struct gaba_diff_vec_s diff __attribute__(( aligned(16) ));
	/**
	 * dh: dH[i, j] - geh
	 * dv: dV[i, j] - gev
	 * de: dE[i, j] + gih + dV[i, j] - gev
	 * df: dF[i, j] + giv + dH[i, j] - geh
	 */
	/* calc dh and dv */
	for(int i = 0; i < BW/2; i++) {
		dh[i] = drop_dh;
		dh[BW/2 + i] = raise_dh;
		dv[i] = raise_dv;
		dv[BW/2 + i] = drop_dv;
	}
 	// dh[BW/2-1] += gih;
 	// dv[BW/2] += giv;
 	dh[BW/2] = raise_dh - gih;
 	dv[BW/2] = raise_dv - giv;

	/* calc de and df */
 	for(int i = 0; i < BW/2; i++) {
 		de[i] = raise_de;
 		de[BW/2 + i] = drop_de;
 		df[i] = drop_df;
 		df[BW/2 + i] = raise_df;
 	}
 	de[BW/2] = drop_de;
 	df[BW/2] = drop_df;

 	/* negate dh */
 	#if 0
 	for(int i = 0; i < BW; i++) {
 		dh[i] = -dh[i];
 	}
 	#endif

 	vec_t _dh = _load(dh);
 	vec_t _dv = _load(dv);
 	vec_t _de = _load(de);
 	vec_t _df = _load(df);

 	_print(_dh);
 	_print(_dv);
 	_print(_de);
 	_print(_df);

	_dh = _shl(_dh, 3);
	_dv = _shl(_dv, 3);
	_store(&diff.dh, _add(_dh, _de));
	_store(&diff.dv, _add(_dv, _df));
	_print(_add(_dh, _de));
	_print(_add(_dv, _df));

	return(diff);
}

/**
 * @fn gaba_init_create_char_vector
 */
static _force_inline
struct gaba_char_vec_s gaba_init_create_char_vector(
	void)
{
	struct gaba_char_vec_s ch;

	for(int i = 0; i < BW; i++) {
		ch.w[i] = 0x80;
	}
	return(ch);
}

/**
 * @fn gaba_init
 */
gaba_t *gaba_init(
	struct gaba_params_s const *params)
{
	/* sequence reader table */
	void (*const rd_seq_a_table[3][7])(
		uint8_t *dst,
		uint8_t const *src,
		uint64_t idx,
		uint64_t src_len,
		uint64_t copy_len) = {
		[GABA_FW_ONLY] = {
			[GABA_ASCII] = _loada_ascii_2bit_fw
		},
		[GABA_FW_RV] = {
			[GABA_ASCII] = _loada_ascii_2bit_fr
		}
	};
	void (*const rd_seq_b_table[3][7])(
		uint8_t *dst,
		uint8_t const *src,
		uint64_t idx,
		uint64_t src_len,
		uint64_t copy_len) = {
		[GABA_FW_ONLY] = {
			[GABA_ASCII] = _loadb_ascii_2bit_fw
		},
		[GABA_FW_RV] = {
			[GABA_ASCII] = _loadb_ascii_2bit_fr
		}
	};

	if(params == NULL) {
		debug("params must not be NULL");
		return(NULL);
	}

	/* copy params to local stack */
	struct gaba_params_s params_intl = *params;

	/* restore defaults */
	gaba_init_restore_default_params(&params_intl);

	/* malloc gaba_context_s */
	struct gaba_context_s *ctx = (struct gaba_context_s *)gaba_aligned_malloc(
		sizeof(struct gaba_context_s),
		MEM_ALIGN_SIZE);
	if(ctx == NULL) {
		return(NULL);
	}

	/* fill context */
	*ctx = (struct gaba_context_s) {
		/* template */
		.k = (struct gaba_dp_context_s) {
			.stack_top = NULL,						/* stored on init */
			.stack_end = NULL,						/* stored on init */

			.r = (struct gaba_reader_s) {
				.loada = rd_seq_a_table
					[params_intl.seq_a_direction]
					[params_intl.seq_a_format],		/* seq a reader */
				.loadb = rd_seq_b_table
					[params_intl.seq_b_direction]
					[params_intl.seq_b_format]		/* seq b reader */
			},

			.scv = gaba_init_create_score_vector(params_intl.score_matrix),
			.tx = params_intl.xdrop,

			.tail = &ctx->tail,

			.mem_cnt = 0,
			.mem_size = MEM_INIT_SIZE,
			.mem_array = { 0 },						/* NULL */
		},
		.md = gaba_init_create_middle_delta(params_intl.score_matrix),
		.blk = (struct gaba_phantom_block_s) {
			.dir = gaba_init_create_dir_dynamic(params_intl.score_matrix),
			.offset = 0,
			.diff = gaba_init_create_diff_vectors(params_intl.score_matrix),
			.sd = gaba_init_create_small_delta(params_intl.score_matrix),
			.ch = gaba_init_create_char_vector(),
			.aridx = 0,
			.bridx = 0
		},
		.tail = (struct gaba_joint_tail_s) {
			.v = &ctx->md,
			.psum = PSUM_BASE - BW,
			.p = 0,
			.ssum = 0,
			.max = 0,
			.stat = CONT,
			.rem_len = 0,
			.tail = NULL,
			.apos = 0,
			.bpos = 0,
			.a = { 0 },
			.b = { 0 }
		},
		.params = params_intl
	};

	return((gaba_t *)ctx);
}

/**
 * @fn gaba_clean
 */
void gaba_clean(
	struct gaba_context_s *ctx)
{
	gaba_aligned_free(ctx);
	return;
}

/**
 * @fn gaba_dp_init
 */
struct gaba_dp_context_s *gaba_dp_init(
	struct gaba_context_s const *ctx,
	struct gaba_seq_pair_s const *p)
{
	/* malloc stack memory */
	struct gaba_dp_context_s *this = (struct gaba_dp_context_s *)gaba_aligned_malloc(
		ctx->k.mem_size,
		MEM_ALIGN_SIZE);
	if(this == NULL) {
		debug("failed to malloc memory");
		return(NULL);
	}

	/* init seq pointers */
	_memcpy_blk_au(
		&this->rr.p,
		p,
		sizeof(struct gaba_seq_pair_s));

	/* copy template */
	_memcpy_blk_aa(
		(uint8_t *)this + GABA_DP_CONTEXT_LOAD_OFFSET,
		(uint8_t *)&ctx->k + GABA_DP_CONTEXT_LOAD_OFFSET,
		GABA_DP_CONTEXT_LOAD_SIZE);

	/* init stack pointers */
	this->stack_top = (uint8_t *)this + sizeof(struct gaba_dp_context_s);
	this->stack_end = (uint8_t *)this + this->mem_size - MEM_MARGIN_SIZE;
	// this->pdr = guide;
	// this->tdr = guide + glen;
	// this->tail = (struct gaba_joint_tail_s *)((uint8_t *)this + (dp_sz + ph_sz));

	return(this);
}

/**
 * @fn gaba_dp_add_stack
 */
static _force_inline
int32_t gaba_dp_add_stack(
	struct gaba_dp_context_s *this)
{
	uint8_t *ptr = this->mem_array[this->mem_cnt];
	uint64_t size = MEM_INIT_SIZE<<(this->mem_cnt + 1);
	if(ptr == NULL) {
		ptr = (uint8_t *)gaba_aligned_malloc(size, MEM_ALIGN_SIZE);
		debug("ptr(%p)", ptr);
	}
	if(ptr == NULL) {
		return(GABA_ERROR_OUT_OF_MEM);
	}
	this->mem_array[this->mem_cnt++] = this->stack_top = ptr;
	this->stack_end = this->stack_top + size - MEM_MARGIN_SIZE;
	debug("stack_top(%p), stack_end(%p), size(%llu)",
		this->stack_top, this->stack_end, size);
	return(GABA_SUCCESS);
}

/**
 * @fn gaba_dp_flush
 */
void gaba_dp_flush(
	struct gaba_dp_context_s *this,
	struct gaba_seq_pair_s const *p)
{
	uint64_t const dp_sz = sizeof(struct gaba_dp_context_s);
	uint64_t const ph_sz = sizeof(struct gaba_phantom_block_s);
	uint64_t const tl_sz = sizeof(struct gaba_joint_tail_s);

	_memcpy_blk_au(
		&this->rr.p,
		p,
		sizeof(struct gaba_seq_pair_s));

	this->stack_top = (uint8_t *)this + (dp_sz + ph_sz + tl_sz);
	this->stack_end = (uint8_t *)this + this->mem_size - MEM_MARGIN_SIZE;
	// this->pdr = guide;
	// this->tdr = guide + glen;

	/* clear memory */
	this->mem_cnt = 0;
	return;
}

/**
 * @fn gaba_dp_malloc
 */
static _force_inline
void *gaba_dp_malloc(
	struct gaba_dp_context_s *this,
	uint64_t size)
{
	/* roundup */
	size = roundup(size, 16);

	/* malloc */
	debug("this(%p), stack_top(%p), size(%llu)", this, this->stack_top, size);
	if((this->stack_end - this->stack_top) < size) {
		if(this->mem_size < size) { this->mem_size = size; }
		if(gaba_dp_add_stack(this) != GABA_SUCCESS) {
			return(NULL);
		}
		debug("stack_top(%p)", this->stack_top);
	}
	this->stack_top += size;
	return((void *)(this->stack_top - size));
}

/**
 * @fn gaba_dp_smalloc
 * @brief small malloc without boundary check, for joint_head, joint_tail, merge_tail, and delta vectors.
 */
static _force_inline
void *gaba_dp_smalloc(
	struct gaba_dp_context_s *this,
	uint64_t size)
{
	debug("this(%p), stack_top(%p)", this, this->stack_top);
	return((void *)((this->stack_top += roundup(size, 16)) - roundup(size, 16)));
}

/**
 * @fn gaba_dp_clean
 */
void gaba_dp_clean(
	struct gaba_dp_context_s *this)
{
	if(this == NULL) {
		return;
	}

	for(uint64_t i = 0; i < GABA_MEM_ARRAY_SIZE; i++) {
		gaba_aligned_free(this->mem_array[i]); this->mem_array[i] = NULL;
	}
	gaba_aligned_free(this);
	return;
}

/* unittests */
#ifdef TEST
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * @fn unittest_build_context
 * @brief build context for unittest
 */
static
struct gaba_score_s const *unittest_default_score_matrix = GABA_SCORE_SIMPLE(2, 3, 5, 1);
static
void *unittest_build_context(void *params)
{
	struct gaba_score_s const *score = (params != NULL)
		? (struct gaba_score_s const *)params
		: unittest_default_score_matrix;

	/* build context */
	gaba_t *ctx = gaba_init(GABA_PARAMS(
		.seq_a_format = GABA_ASCII,
		.seq_a_direction = GABA_FW_ONLY,
		.seq_b_format = GABA_ASCII,
		.seq_b_direction = GABA_FW_ONLY,
		.xdrop = 100,
		.score_matrix = score));
	return((void *)ctx);
}

/**
 * @fn unittest_clean_context
 */
static
void unittest_clean_context(void *ctx)
{
	gaba_clean((struct gaba_context_s *)ctx);
	return;
}

/**
 * @struct unittest_seqs_s
 * @brief sequence container
 */
struct unittest_seqs_s {
	char const *a;
	char const *b;
};
#define unittest_seq_pair(_a, _b) ( \
	(void *)&((struct unittest_seqs_s const){ \
		.a = _a "GGGGGGGGGGGGGGGGGGGG", \
		.b = _b "CCCCCCCCCCCCCCCCCCCC" \
	}) \
)

/**
 * @struct unittest_sections_s
 * @brief section container
 */
struct unittest_sections_s {
	struct gaba_seq_pair_s seq;
	struct gaba_section_s afsec, aftail, bfsec, bftail;
	struct gaba_section_s arsec, artail, brsec, brtail;
};

/**
 * @fn unittest_build_seqs
 * @brief build seq_pair and sections
 */
static
void *unittest_build_seqs(void *params)
{
	struct unittest_seqs_s const *seqs = (struct unittest_seqs_s const *)params;
	char const *a = seqs->a;
	char const *b = seqs->b;

	uint64_t alim = strlen(a);
	uint64_t blim = strlen(b);
	uint32_t alen = alim - 20;
	uint32_t blen = blim - 20;

	int64_t margin = 64;
	struct unittest_sections_s *sec = malloc(
		sizeof(struct unittest_sections_s) + (alim + 1) + (blim + 1) + margin);

	/* copy sequences */
	char *ca = (char *)(sec + 1);
	char *cb = ca + alim + 1;
	char *cm = cb + blim + 1;
	strcpy(ca, a);
	strcpy(cb, b);
	memset(cm, 0, margin);

	/* build context */
	*sec = (struct unittest_sections_s){
		.seq = gaba_build_seq_pair(ca, alim, cb, blim),
		
		/* forward */
		.afsec = gaba_build_section(1, 0, alen),
		.aftail = gaba_build_section(2, alen, 20),
		.bfsec = gaba_build_section(3, 0, blen),
		.bftail = gaba_build_section(4, blen, 20),

		/* reverse */
		.arsec = gaba_build_section(-1, 2 * alim - alen, alen),
		.artail = gaba_build_section(-2, alim, 20),
		.brsec = gaba_build_section(-3, 2 * blim - blen, blen),
		.brtail = gaba_build_section(-4, blim, 20)
	};
	return((void *)sec);
}

/**
 * @fn unittest_clean_seqs
 */
static
void unittest_clean_seqs(void *ctx)
{
	free(ctx);
}

/**
 * @macro with_seq_pair
 * @brief constructs .param, .init, and .clean parameters
 */
#define with_seq_pair(_a, _b) \
	.params = unittest_seq_pair(_a, _b), \
	.init = unittest_build_seqs, \
	.clean = unittest_clean_seqs

/**
 * misc macros and functions for assertion
 */
#define check_tail(_t, _max, _p, _psum, _ssum) ( \
	   (_t) != NULL \
	&& (_t)->max == (_max) \
	&& (_t)->p == (_p) \
	&& (_t)->psum == (_psum) \
	&& (_t)->ssum == (_ssum) \
)
#define print_tail(_t) \
	"tail(%p), max(%lld), p(%d), psum(%lld), ssum(%lld)", \
	(_t), (_t)->max, (_t)->p, (_t)->psum, (_t)->ssum
#define check_result(_r, _score, _plen, _rem, _slen) ( \
	   (_r) != NULL \
	&& (_r)->sec != NULL \
	&& (_r)->path != NULL \
	&& (_r)->path->len == (_plen) \
	&& (_r)->path->rem == (_rem) \
	&& (_r)->slen == (_slen) \
	&& (_r)->score == (_score) \
)
#define print_result(_r) \
	"res(%p), score(%lld), plen(%u), rem(%u), slen(%u)", \
	(_r), (_r)->score, (_r)->path->len, (_r)->path->rem, (_r)->slen

static
int check_path(
	struct gaba_result_s const *res,
	char const *str)
{
	int64_t plen = res->path->len, slen = strlen(str);
	uint32_t const *p = &res->path->array[roundup(plen, 32) / 32 - 1];
	char const *s = &str[slen - 1];
	debug("%s", str);
	if(plen != slen) { return(0); }
	while(plen > 0) {
		int64_t i = 0;
		uint32_t array = 0;
		for(i = 0; i < 32; i++) {
			if(plen-- == 0) { break; }
			array = (array<<1) | ((*s-- == 'D') ? 1 : 0);
			debug("%c, %x", s[1], array);
		}
		array <<= 32 - i;
		debug("path(%x), array(%x), i(%lld)", *p, array, i);
		if(*p-- != array) {
			return(0);
		}
	}
	return(1);
}

#define decode_path(_r) ({ \
	int64_t plen = (_r)->path->len, cnt = 0; \
	uint32_t const *path = &(_r)->path->array[roundup(plen, 32) / 32 - 1]; \
	uint32_t path_array = *path; \
	char *p = alloca(plen) + plen; \
	*p-- = '\0'; \
	while(plen-- > 0) { \
		*p-- = (path_array & 0x80000000) ? 'D' : 'R'; \
		path_array <<= 1; \
		if(++cnt == 32) { \
			path_array = *--path; \
			cnt = 0; \
		} \
	} \
	(p + 1); \
})
#define print_path(_r)			"%s", decode_path(_r)
#define eq_section(_s1, _s2) \
	((_s1).id == (_s2).id && (_s1).len == (_s2).len && (_s1).base == (_s2).base)
#define check_section(_s, _a, _apos, _alen, _b, _bpos, _blen) \
	(eq_section((_s).a, _a) && (_s).apos == (_apos) && (_s).alen == (_alen) \
	&& eq_section((_s).b, _b) && (_s).bpos == (_bpos) && (_s).blen == (_blen))
#define print_section(_s) \
	"a(%d, %llu, %u), apos(%u), alen(%u), b(%d, %llu, %u), bpos(%u), blen(%u)", \
	(_s).a.id, (_s).a.base, (_s).a.len, (_s).apos, (_s).alen, \
	(_s).b.id, (_s).b.base, (_s).b.len, (_s).bpos, (_s).blen

/* global configuration of the tests */
unittest_config(
	.name = "sea",
	.init = unittest_build_context,
	.clean = unittest_clean_context
);

/**
 * check if gaba_init returns a valid pointer to a context
 */
unittest()
{
	struct gaba_context_s *c = (struct gaba_context_s *)gctx;
	assert(c != NULL, "%p", c);
}

/**
 * check if unittest_build_seqs returns a valid seq_pair and sections
 */
unittest(with_seq_pair("A", "A"))
{
	struct unittest_sections_s *s = (struct unittest_sections_s *)ctx;

	/* check pointer */
	assert(s != NULL, "%p", s);

	/* check sequences */
	assert(strcmp(s->seq.a, "AGGGGGGGGGGGGGGGGGGGG") == 0, "%s", s->seq.a);
	assert(strcmp(s->seq.b, "ACCCCCCCCCCCCCCCCCCCC") == 0, "%s", s->seq.b);
	assert(s->seq.alen == 21, "%llu", s->seq.alen);
	assert(s->seq.blen == 21, "%llu", s->seq.blen);

	/* check fowrard sections */
	assert(s->afsec.id == 1, "%d", s->afsec.id);
	assert(s->afsec.base == 0, "%llu", s->afsec.base);
	assert(s->afsec.len == 1, "%u", s->afsec.len);

	assert(s->aftail.id == 2, "%d", s->aftail.id);
	assert(s->aftail.base == 1, "%llu", s->aftail.base);
	assert(s->aftail.len == 20, "%u", s->aftail.len);

	assert(s->bfsec.id == 3, "%d", s->bfsec.id);
	assert(s->bfsec.base == 0, "%llu", s->bfsec.base);
	assert(s->bfsec.len == 1, "%u", s->bfsec.len);

	assert(s->bftail.id == 4, "%d", s->bftail.id);
	assert(s->bftail.base == 1, "%llu", s->bftail.base);
	assert(s->bftail.len == 20, "%u", s->bftail.len);

	/* check reverse sections */
	assert(s->arsec.id == -1, "%d", s->arsec.id);
	assert(s->arsec.base == 41, "%llu", s->arsec.base);
	assert(s->arsec.len == 1, "%u", s->arsec.len);

	assert(s->artail.id == -2, "%d", s->artail.id);
	assert(s->artail.base == 21, "%llu", s->artail.base);
	assert(s->artail.len == 20, "%u", s->artail.len);

	assert(s->brsec.id == -3, "%d", s->brsec.id);
	assert(s->brsec.base == 41, "%llu", s->brsec.base);
	assert(s->brsec.len == 1, "%u", s->brsec.len);

	assert(s->brtail.id == -4, "%d", s->brtail.id);
	assert(s->brtail.base == 21, "%llu", s->brtail.base);
	assert(s->brtail.len == 20, "%u", s->brtail.len);
}

/**
 * check if gaba_dp_init returns a vaild pointer to a dp context
 */
#define omajinai() \
	struct gaba_context_s const *c = (struct gaba_context_s const *)gctx; \
	struct unittest_sections_s *s = (struct unittest_sections_s *)ctx; \
	struct gaba_dp_context_s *d = gaba_dp_init(c, &s->seq);

unittest(with_seq_pair("A", "A"))
{
	omajinai();

	assert(d != NULL, "%p", d);
	gaba_dp_clean(d);
}

/**
 * check if gaba_dp_fill_root and gaba_dp_fill returns a correct score
 */
unittest(with_seq_pair("A", "A"))
{
	omajinai();

	/* fill root section */
	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	assert(f->status == 0x1ff, "%x", f->status);
	assert(check_tail(f, 0, 0, -29, 1), print_tail(f));

	/* fill again */
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	assert(f->status == 0x1ff, "%x", f->status);
	assert(check_tail(f, 0, 0, -27, 2), print_tail(f));

	/* fill tail section */
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);
	assert(f->status == 0x1ff, "%x", f->status);
	assert(check_tail(f, 4, 13, 13, 3), print_tail(f));

	/* fill tail section again */
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);
	assert(f->status == 0x10f, "%x", f->status);
	assert(check_tail(f, -11, 31, 44, 4), print_tail(f));

	gaba_dp_clean(d);
}

/* with longer sequences */
unittest(with_seq_pair("ACGTACGTACGT", "ACGTACGTACGT"))
{
	omajinai();

	/* fill root section */
	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	assert(f->status == 0x1ff, "%x", f->status);
	assert(check_tail(f, 0, 0, -7, 1), print_tail(f));

	/* fill again */
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	assert(f->status == 0x1ff, "%x", f->status);
	assert(check_tail(f, 16, 17, 17, 2), print_tail(f));

	/* fill tail section */
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);
	assert(f->status == 0x1ff, "%x", f->status);
	assert(check_tail(f, 48, 40, 57, 3), print_tail(f));

	/* fill tail section again */
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);
	assert(f->status == 0x10f, "%x", f->status);
	assert(check_tail(f, 33, 31, 88, 4), print_tail(f));

	gaba_dp_clean(d);
}

/* sequences with different lengths (consumed as mismatches) */
unittest(with_seq_pair("GAAAAAAAA", "AAAAAAAA"))
{
	omajinai();

	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	assert(f->status == 0x01ff, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	assert(f->status == 0x01f0, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bftail);
	assert(f->status == 0x010f, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);
	assert(f->status == 0x01f0, "%x", f->status);
	assert(check_tail(f, 22, 37, 42, 4), print_tail(f));

	gaba_dp_clean(d);
}

/* another pair with different lengths */
unittest(with_seq_pair("TTTTTTTT", "CTTTTTTTT"))
{
	omajinai();

	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	assert(f->status == 0x010f, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	assert(f->status == 0x01f0, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	assert(f->status == 0x010f, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bfsec);
	assert(f->status == 0x01f0, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);
	assert(f->status == 0x010f, "%x", f->status);
	assert(check_tail(f, 22, 35, 41, 5), print_tail(f));

	gaba_dp_clean(d);
}

/* with deletions */
unittest(with_seq_pair("GACGTACGT", "ACGTACGT"))
{
	omajinai();

	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	assert(f->status == 0x01ff, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	assert(f->status == 0x01f0, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bftail);
	assert(f->status == 0x010f, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);
	assert(f->status == 0x01ff, "%x", f->status);
	assert(check_tail(f, 20, 38, 43, 4), print_tail(f));

	gaba_dp_clean(d);
}

/* with insertions */
unittest(with_seq_pair("ACGTACGT", "GACGTACGT"))
{
	omajinai();

	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	assert(f->status == 0x010f, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	assert(f->status == 0x01f0, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	assert(f->status == 0x010f, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bfsec);
	assert(f->status == 0x01f0, "%x", f->status);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);
	assert(f->status == 0x010f, "%x", f->status);
	assert(check_tail(f, 20, 36, 42, 5), print_tail(f));

	gaba_dp_clean(d);
}

/**
 * check if gaba_dp_trace returns a correct path
 */
/* with empty sequences */
unittest(with_seq_pair("A", "A"))
{
	omajinai();

	/* fill sections */
	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);

	/* forward-only traceback */
	struct gaba_result_s *r = gaba_dp_trace(d, f, NULL, NULL);
	assert(check_result(r, 0, 0, 0, 0), print_result(r));

	/* forward-reverse traceback */
	r = gaba_dp_trace(d, f, f, NULL);
	assert(check_result(r, 0, 0, 0, 0), print_result(r));

	/* section added */
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);

	/* forward-only traceback */
	r = gaba_dp_trace(d, f, NULL, NULL);
	assert(check_result(r, 0, 0, 0, 0), print_result(r));

	/* forward-reverse traceback */
	r = gaba_dp_trace(d, f, f, NULL);
	assert(check_result(r, 0, 0, 0, 0), print_result(r));
}

/* with short sequences */
unittest(with_seq_pair("A", "A"))
{
	omajinai();

	/* fill sections */
	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);

	/* forward-only traceback */
	struct gaba_result_s *r = gaba_dp_trace(d, f, NULL, NULL);
	assert(check_result(r, 4, 4, 4, 2), print_result(r));
	assert(check_path(r, "DRDR"), print_path(r));
	assert(check_section(r->sec[0], s->afsec, 0, 1, s->bfsec, 0, 1), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->afsec, 0, 1, s->bfsec, 0, 1), print_section(r->sec[1]));

	/* reverse-only traceback */
	r = gaba_dp_trace(d, NULL, f, NULL);
	assert(check_result(r, 4, 4, 4, 2), print_result(r));
	assert(check_path(r, "DRDR"), print_path(r));
	assert(check_section(r->sec[0], s->arsec, 0, 1, s->brsec, 0, 1), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 0, 1, s->brsec, 0, 1), print_section(r->sec[1]));

	/* forward-reverse traceback */
	r = gaba_dp_trace(d, f, f, NULL);
	assert(check_result(r, 8, 8, 8, 4), print_result(r));
	assert(check_path(r, "DRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->arsec, 0, 1, s->brsec, 0, 1), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 0, 1, s->brsec, 0, 1), print_section(r->sec[1]));
	assert(check_section(r->sec[2], s->afsec, 0, 1, s->bfsec, 0, 1), print_section(r->sec[2]));
	assert(check_section(r->sec[3], s->afsec, 0, 1, s->bfsec, 0, 1), print_section(r->sec[3]));

	gaba_dp_clean(d);
}

/* with longer sequences */
unittest(with_seq_pair("ACGTACGTACGT", "ACGTACGTACGT"))
{
	omajinai();

	/* fill sections */
	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);

	/* fw */
	struct gaba_result_s *r = gaba_dp_trace(d, f, NULL, NULL);
	assert(check_result(r, 48, 48, 16, 2), print_result(r));
	assert(check_path(r, "DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->afsec, 0, 12, s->bfsec, 0, 12), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->afsec, 0, 12, s->bfsec, 0, 12), print_section(r->sec[1]));

	/* rv */
	r = gaba_dp_trace(d, NULL, f, NULL);
	assert(check_result(r, 48, 48, 16, 2), print_result(r));
	assert(check_path(r, "DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->arsec, 0, 12, s->brsec, 0, 12), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 0, 12, s->brsec, 0, 12), print_section(r->sec[1]));

	/* fw-rv */
	r = gaba_dp_trace(d, f, f, NULL);
	assert(check_result(r, 96, 96, 0, 4), print_result(r));
	assert(check_path(r,
		"DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"
		"DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"),
		print_path(r));
	assert(check_section(r->sec[0], s->arsec, 0, 12, s->brsec, 0, 12), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 0, 12, s->brsec, 0, 12), print_section(r->sec[1]));
	assert(check_section(r->sec[2], s->afsec, 0, 12, s->bfsec, 0, 12), print_section(r->sec[2]));
	assert(check_section(r->sec[3], s->afsec, 0, 12, s->bfsec, 0, 12), print_section(r->sec[3]));

	gaba_dp_clean(d);
}

/* sequences with different lengths (consumed as mismatches) */
unittest(with_seq_pair("GAAAAAAAA", "AAAAAAAA"))
{
	omajinai();

	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bftail);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);

	/* fw */	
	struct gaba_result_s *r = gaba_dp_trace(d, f, NULL, NULL);
	assert(check_result(r, 22, 32, 0, 3), print_result(r));
	assert(check_path(r, "DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->afsec, 0, 8, s->bfsec, 0, 8), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->afsec, 8, 1, s->bfsec, 0, 1), print_section(r->sec[1]));
	assert(check_section(r->sec[2], s->afsec, 0, 7, s->bfsec, 1, 7), print_section(r->sec[2]));

	/* rv */
	r = gaba_dp_trace(d, NULL, f, NULL);
	assert(check_result(r, 22, 32, 0, 3), print_result(r));
	assert(check_path(r, "DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->arsec, 2, 7, s->brsec, 0, 7), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 0, 1, s->brsec, 7, 1), print_section(r->sec[1]));
	assert(check_section(r->sec[2], s->arsec, 1, 8, s->brsec, 0, 8), print_section(r->sec[2]));

	/* fw-rv */
	r = gaba_dp_trace(d, f, f, NULL);
	assert(check_result(r, 44, 64, 0, 6), print_result(r));
	assert(check_path(r,
		"DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"
		"DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"),
		print_path(r));
	assert(check_section(r->sec[0], s->arsec, 2, 7, s->brsec, 0, 7), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 0, 1, s->brsec, 7, 1), print_section(r->sec[1]));
	assert(check_section(r->sec[2], s->arsec, 1, 8, s->brsec, 0, 8), print_section(r->sec[2]));
	assert(check_section(r->sec[3], s->afsec, 0, 8, s->bfsec, 0, 8), print_section(r->sec[3]));
	assert(check_section(r->sec[4], s->afsec, 8, 1, s->bfsec, 0, 1), print_section(r->sec[4]));
	assert(check_section(r->sec[5], s->afsec, 0, 7, s->bfsec, 1, 7), print_section(r->sec[5]));

	gaba_dp_clean(d);
}

/* another pair with different lengths */
unittest(with_seq_pair("TTTTTTTT", "CTTTTTTTT"))
{
	omajinai();

	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bfsec);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);

	/* fw */
	struct gaba_result_s *r = gaba_dp_trace(d, f, NULL, NULL);
	assert(check_result(r, 22, 32, 0, 3), print_result(r));
	assert(check_path(r, "DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->afsec, 0, 8, s->bfsec, 0, 8), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->afsec, 0, 1, s->bfsec, 8, 1), print_section(r->sec[1]));
	assert(check_section(r->sec[2], s->afsec, 1, 7, s->bfsec, 0, 7), print_section(r->sec[2]));

	/* rv */
	r = gaba_dp_trace(d, NULL, f, NULL);
	assert(check_result(r, 22, 32, 0, 3), print_result(r));
	assert(check_path(r, "DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->arsec, 0, 7, s->brsec, 2, 7), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 7, 1, s->brsec, 0, 1), print_section(r->sec[1]));
	assert(check_section(r->sec[2], s->arsec, 0, 8, s->brsec, 1, 8), print_section(r->sec[2]));

	/* fw-rv */
	r = gaba_dp_trace(d, f, f, NULL);
	assert(check_result(r, 44, 64, 0, 6), print_result(r));
	assert(check_path(r,
		"DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"
		"DRDRDRDRDRDRDRDRDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->arsec, 0, 7, s->brsec, 2, 7), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 7, 1, s->brsec, 0, 1), print_section(r->sec[1]));
	assert(check_section(r->sec[2], s->arsec, 0, 8, s->brsec, 1, 8), print_section(r->sec[2]));
	assert(check_section(r->sec[3], s->afsec, 0, 8, s->bfsec, 0, 8), print_section(r->sec[3]));
	assert(check_section(r->sec[4], s->afsec, 0, 1, s->bfsec, 8, 1), print_section(r->sec[4]));
	assert(check_section(r->sec[5], s->afsec, 1, 7, s->bfsec, 0, 7), print_section(r->sec[5]));

	gaba_dp_clean(d);
}

/* with deletions */
unittest(with_seq_pair("GACGTACGT", "ACGTACGT"))
{
	omajinai();

	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bftail);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);

	/* fw */
	struct gaba_result_s *r = gaba_dp_trace(d, f, NULL, NULL);
	assert(check_result(r, 20, 34, 2, 2), print_result(r));
	assert(check_path(r, "RDRDRDRDRDRDRDRDRRDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->afsec, 0, 9, s->bfsec, 0, 8), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->afsec, 0, 9, s->bfsec, 0, 8), print_section(r->sec[1]));

	/* rv */
	r = gaba_dp_trace(d, NULL, f, NULL);
	assert(check_result(r, 20, 34, 2, 2), print_result(r));
	assert(check_path(r, "DRDRDRDRDRDRDRDRRDRDRDRDRDRDRDRDRR"), print_path(r));
	assert(check_section(r->sec[0], s->arsec, 0, 9, s->brsec, 0, 8), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 0, 9, s->brsec, 0, 8), print_section(r->sec[1]));

	/* fw-rv */
	r = gaba_dp_trace(d, f, f, NULL);
	/* fixme!! continuous gaps at the root must be concatenated! */
	assert(check_result(r, 40, 68, 4, 4), print_result(r));
	assert(check_path(r,
		"DRDRDRDRDRDRDRDRRDRDRDRDRDRDRDRDRR"
		"RDRDRDRDRDRDRDRDRRDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->arsec, 0, 9, s->brsec, 0, 8), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 0, 9, s->brsec, 0, 8), print_section(r->sec[1]));
	assert(check_section(r->sec[2], s->afsec, 0, 9, s->bfsec, 0, 8), print_section(r->sec[2]));
	assert(check_section(r->sec[3], s->afsec, 0, 9, s->bfsec, 0, 8), print_section(r->sec[3]));

	gaba_dp_clean(d);
}

/* with insertions */
unittest(with_seq_pair("ACGTACGT", "GACGTACGT"))
{
	omajinai();

	struct gaba_fill_s *f = gaba_dp_fill_root(d, &s->afsec, 0, &s->bfsec, 0);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	f = gaba_dp_fill(d, f, &s->afsec, &s->bfsec);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bfsec);
	f = gaba_dp_fill(d, f, &s->aftail, &s->bftail);

	/* fw */
	struct gaba_result_s *r = gaba_dp_trace(d, f, NULL, NULL);
	assert(check_result(r, 20, 34, 2, 2), print_result(r));
	assert(check_path(r, "DDRDRDRDRDRDRDRDRDDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->afsec, 0, 8, s->bfsec, 0, 9), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->afsec, 0, 8, s->bfsec, 0, 9), print_section(r->sec[1]));

	/* rv */
	r = gaba_dp_trace(d, NULL, f, NULL);
	assert(check_result(r, 20, 34, 2, 2), print_result(r));
	assert(check_path(r, "DRDRDRDRDRDRDRDRDDRDRDRDRDRDRDRDRD"), print_path(r));
	assert(check_section(r->sec[0], s->arsec, 0, 8, s->brsec, 0, 9), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 0, 8, s->brsec, 0, 9), print_section(r->sec[1]));

	/* fw-rv */
	r = gaba_dp_trace(d, f, f, NULL);
	assert(check_result(r, 40, 68, 4, 4), print_result(r));
	assert(check_path(r,
		"DRDRDRDRDRDRDRDRDDRDRDRDRDRDRDRDRD"
		"DDRDRDRDRDRDRDRDRDDRDRDRDRDRDRDRDR"), print_path(r));
	assert(check_section(r->sec[0], s->arsec, 0, 8, s->brsec, 0, 9), print_section(r->sec[0]));
	assert(check_section(r->sec[1], s->arsec, 0, 8, s->brsec, 0, 9), print_section(r->sec[1]));
	assert(check_section(r->sec[2], s->afsec, 0, 8, s->bfsec, 0, 9), print_section(r->sec[2]));
	assert(check_section(r->sec[3], s->afsec, 0, 8, s->bfsec, 0, 9), print_section(r->sec[3]));

	gaba_dp_clean(d);
}

/* cross tests */

/**
 * @struct unittest_naive_result_s
 * @brief result container
 */
struct unittest_naive_result_s {
	int32_t score;
	uint32_t path_length;
	uint64_t apos, bpos;
	uint64_t alen, blen;
	char *path;
};

/**
 * @fn unittest_naive_encode_a
 */
static inline
int8_t unittest_naive_encode(char a)
{
	return(0x03 & ((a>>1) ^ (a>>2)));
}

/**
 * @fn unittest_naive
 *
 * @brief naive implementation of the forward semi-global alignment algorithm
 * left-aligned gap and left-aligned deletion
 */
static
struct unittest_naive_result_s unittest_naive(
	struct gaba_score_s const *sc,
	char const *a,
	char const *b)
{
	/* utils */
	#define _a(p, q, plen)		( (q) * ((plen) + 1) + (p) )
	#define s(p, q)		_a(p, 3*(q), alen)
	#define e(p, q)		_a(p, 3*(q)+1, alen)
	#define f(p, q)		_a(p, 3*(q)+2, alen)
	// #define s(p, q)		( (a[(p) - 1] == b[(q) - 1]) ? m : x )
	#define m(p, q)		( \
		sc->score_sub \
			[unittest_naive_encode(a[(p) - 1])] \
			[unittest_naive_encode(b[(q) - 1])] \
	)

	/* load gap penalties */
	int8_t gia = -sc->score_gi_a;
	int8_t gib = -sc->score_gi_b;
	int8_t gea = -sc->score_ge_a;
	int8_t geb = -sc->score_ge_a;

	/* calc lengths */
	uint64_t alen = strlen(a);
	uint64_t blen = strlen(b);

	/* calc min */
	int8_t *v = (int8_t *)sc->score_sub;
	int16_t min = 0;
	for(int i = 0; i < 16; i++) {
		min = (v[i] < min) ? v[i] : min;
	}
	min = INT16_MIN - min - gia - gib;

	/* malloc matrix */
	int16_t *mat = (int16_t *)malloc(
		3 * (alen + 1) * (blen + 1) * sizeof(int16_t));

	/* init */
	struct unittest_naive_maxpos_s {
		int16_t score;
		uint64_t apos;
		uint64_t bpos;
	};

	struct unittest_naive_maxpos_s max = { 0, 0, 0 };
	mat[s(0, 0)] = mat[e(0, 0)] = mat[f(0, 0)] = 0;
	for(uint64_t i = 1; i < alen+1; i++) {
		mat[s(i, 0)] = mat[e(i, 0)] = MAX2(min, gia + i * gea);
		mat[f(i, 0)] = MAX2(min, gia + i * gea + gib - 1);
	}
	for(uint64_t j = 1; j < blen+1; j++) {
		mat[s(0, j)] = mat[f(0, j)] = MAX2(min, gib + j * geb);
		mat[e(0, j)] = MAX2(min, gib + j * geb + gia - 1);
	}

	for(uint64_t j = 1; j < blen+1; j++) {
		for(uint64_t i = 1; i < alen+1; i++) {
			int16_t score_e = mat[e(i, j)] = MAX2(
				mat[s(i - 1, j)] + gia + gea,
				mat[e(i - 1, j)] + gea);
			int16_t score_f = mat[f(i, j)] = MAX2(
				mat[s(i, j - 1)] + gib + geb,
				mat[f(i, j - 1)] + geb);
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
		.path = (char *)malloc(max.apos + max.bpos + 1)
	};
	uint32_t path_index = max.apos + max.bpos + 1;
	while(max.apos > 0 || max.bpos > 0) {
		/* M > I > D > X */
		if(mat[s(max.apos, max.bpos)] == mat[f(max.apos, max.bpos)]) {
			max.bpos--;
			result.path[--path_index] = 'D';
		} else if(mat[s(max.apos, max.bpos)] == mat[e(max.apos, max.bpos)]) {
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

	result.path_length -= path_index;
	for(uint64_t i = 0; i < result.path_length; i++) {
		result.path[i] = result.path[path_index++];
	}
	result.path[result.path_length] = '\0';

	free(mat);

	#undef a
	#undef f
	#undef e
	#undef s
	return(result);
}

/**
 * @fn unittest_random_base
 */
static _force_inline
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
	seq = (char *)malloc(sizeof(char) * (len + 1));

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
	int64_t wave = 0;			/** wave is q-coordinate of the alignment path */
	int64_t len = strlen(seq);
	char *mutated_seq;

	if(seq == NULL) { return NULL; }
	mutated_seq = (char *)malloc(sizeof(char) * (len + 1));
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
char *unittest_add_tail(
	char *seq,
	char c,
	int64_t tail_len)
{
	int64_t len = strlen(seq);
	seq = realloc(seq, len + tail_len + 1);

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
	"score(%lld), path(%s), len(%lld)", \
	(_r).score, (_r).path, (_r).path_length

static
char *string_pair_diff(
	char const *a,
	char const *b)
{
	int64_t len = 2 * (strlen(a) + strlen(b));
	char *base = malloc(len);
	char *ptr = base, *tail = base + len - 1;
	int64_t state = 0;

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
		for(int64_t i = 0; i < strlen(str); i++) { \
			push(str[i]); \
		} \
	}

	int64_t i;
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
	for(int64_t i = 0; i < strlen(b); i++) {
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

unittest()
{
	struct gaba_context_s const *c = (struct gaba_context_s const *)gctx;
	struct gaba_score_s const *p = c->params.score_matrix;
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
}
#if 1
/* cross test */
unittest()
{
	struct gaba_context_s const *c = (struct gaba_context_s const *)gctx;
	struct gaba_score_s const *p = c->params.score_matrix;

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
		a = unittest_add_tail(a, 'C', 20);
		b = unittest_add_tail(b, 'G', 20);

		/* build section */
		struct unittest_sections_s *sec = unittest_build_seqs(
			&((struct unittest_seqs_s){ .a = a, .b = b }));

		debug("seed(%d)\n%s", seed, format_string_pair_diff(a, b));

		/* generate dp context */
		struct gaba_dp_context_s *d = gaba_dp_init(c, &sec->seq);

		/* fill section */
		struct gaba_section_s const *as = &sec->afsec;
		struct gaba_section_s const *bs = &sec->bfsec;
		struct gaba_fill_s *f = gaba_dp_fill_root(d, as, 0, bs, 0);
		
		/* fill tail (1) */
		as = (f->status & 0x0f) ? &sec->aftail : as;
		bs = (f->status & 0xf0) ? &sec->bftail : bs;
		struct gaba_fill_s *t1 = gaba_dp_fill(d, f, as, bs);
		f = (t1->max > f->max) ? t1 : f;

		/* fill tail (2) */
		as = (f->status & 0x0f) ? &sec->aftail : as;
		bs = (f->status & 0xf0) ? &sec->bftail : bs;
		struct gaba_fill_s *t2 = gaba_dp_fill(d, t1, as, bs);
		f = (t2->max > f->max) ? t2 : f;

		/* trace */
		struct gaba_result_s *r = gaba_dp_trace(d, f, NULL, NULL);

		/* naive */
		struct unittest_naive_result_s n = unittest_naive(p, a, b);

		/* check scores */
		assert(r->score == n.score, "f->max(%lld), r->score(%lld), n.score(%d)",
			f->max, r->score, n.score);
		assert(check_path(r, n.path), "\n%s\n%s",
			format_string_pair_diff(a, b),
			format_string_pair_diff(decode_path(r), n.path));

		debug("score(%lld, %d), alen(%lld), blen(%lld)\n%s",
			r->score, n.score, n.alen, n.blen,
			format_string_pair_diff(decode_path(r), n.path));

		/* cleanup */
		gaba_dp_clean(d);
		free(sec);
		free(n.path);
		free(a);
		free(b);
	}
}
#else
#if 0
"GACGCCTAATAATGTAGAAGACGGATCGTAACTTGATTGCTCGTTTTACTCATTGAGCAGCCTACAGAAATCGTGCCCCCCGACGGGACTGTAAGCCGTTAGGACATCGTTCTAGTCCTTACTTGCTCCGCCCCCGACGGGCGATCCAGCTCGGTAACAACAAACATAGTTGTACCATCTGCGAGTTTGAGACTTAAGCCGGACGCGCAAGCGGCTGGGCGGTTTAGAAGGATTTACTGACGATTTATATTCTATTCCACCAGTGCTTCAGCACTGGGCGCGTGGGATACGAACATTCAAAGCGGATACTATCCATAGTGAAGGGGCCGTGCTGGTTTAAAGAGGAGGCGTACTATATGTAGCTTGCTTTGCCGCGGGGGCAGCCAGAGGGAACCATGACAGGATAGACACCGGAGTCCCGATCAAACGTCTTCATAGTTTGCATTATCGTCAGCGTCTCGGATAGTCACAGTAGTTAAGTTACTTTCTTAGCACAATGTGTTGGAGTTGGCAATTCTCCTCAGGCCCAGTGATATCCCGGTGTTGACTAAAAGCTTGGAAACCGACTCGAACTCGTGGCCCCTCTGATTAATTCCAGCTAAGGAACGGTTTAGATGCCAAAGGGAGGATCCGTTCCAACACTGGTATACAATATACCGTCGCGACGACCTCGCGAACGGCTGAAAATATGTAGCTATGAGTCCTACTTGGGAGGGACCCAGATCACGCTGACAATTGATAGGGCTGTTGCATTACTCCCCGTCTCGACCTTCGGGGGATCCACCAGAACGTCCCCAGGCCTGAATCTCCAGTCGCTCTGAAGTAGACCCAATAGGTGAATAGGCCCCATTAGACAATAGCTCGCGAAATTTGGGAAACTGGACTCCGCGAGAATATCCGACGGTGACATTCCTATACCCGGAAGCTACCTAAAGCGTTGAGTGGACGTGACGGGCCGTCGCAATCTAAGATCAACCCTCCCGGTTTACGGACTAAGGCTAAAGACGAGTGATACCTACATTGGCGTACCCGGGCTGTGCACAGCTTTACTCGACTATCATCGCCCCCCCCCCCCCCCCCCCCC",
"GACGACCTAATAATGTACAAAACGGCTCGTATGCTAGATTCCCGTGTACTCATTGAAGCAGTCTACAGAAATCTTGCACCACCACGGGACTTACAAGCCGTCTAGGACATCGTTTCTAGTCCTTGACTTGCCCGCCCCCGACGAGCGATCAAGCTCGGTAACAACAAACATAGTTTCACCATCTAGCGACTTTGAGACTAATCCGGACGCGCAAGCGGCTGGGCGAGTTTAAGAGGATATACTGACGATTTATATTCCTTATTCCACCTAGTGATTCAGCACAGTGCGTGTGGGATCGAACATTCGAGCTTGATACTGTCCATAGTGAAGGGACCGTGACTCGTTTAAAGAGGAGGCGTACTATATGTTGCCTGTTGCCGCGGGGCAGCCAAGGGTCCCATGACACCATAAGACACGGAGTCCCGCCAAACGTCTTCATTGTTTGCATCTCGTCCGCGTCTCGGCATAGTCACTGTAGTTAAGTTACTTCTTAGCACATTTTGAGGTTGTGCAATTCTCCCTCGGCCCAGTGGTACCCGGGTTTGTGACTAATCAGCTTGGAAACCGACTCGAACTCGTGGCCCCTCTGATTAATTCCAGCTAAAGGAGACGGTTTAGATGCCCAAGGAGGATCCGTCCAACACTGGTATACAATAATACCGTAGCGACGACCCGCGACACGGATAAAATCTAGCTATGGTCCTACTTGGGAGGGACCCAATCACGGGACAATTGATAGGGCAGTTGCATTACTCCGCGTGACGCCTTCGGGGGATCCACCACAACTCCCCAGGCCTGAATCTCCAGTCGCTCTGAATTAGACCCAGTAGGCAATAGGCCCCGTTAGACAAATAGCTCGCGGAAAGTTTGGGATAACTGGAGCTCCGCGAGAATAACCGCGGTGACATTCCATATCCCGGAAGCTACCTTAAAGCCGCTGAGTGACTGTACGGGCCGTCGGCATCTGTACATCAACCCTCCCGGTTTACGGACTAAGGCTGAATGTCCCCGTAAGGCCTGTGCACTGCTCTTACTCTCCGGACTGTTTGCTCTTATAGTTGTTTGGGGGGGGGGGGGGGGGGGG"
#endif
/* for debugging */
unittest(with_seq_pair(
"TCATACCGACACATTTCTTAAAAGCAAGCAGACTCGGAGGTCGACAGAAGTGATGGTACGCGGACTCCCCTGACGCGCGAACCTGATGCCGCGTGCCACGCCCTCAGGAAAGTATTAACATGAGCATGGTATTTTTTTGACGGCGGAACGGTGTCCAGCCGTGACCTACCCCCGCGACGGTGACAACGGCATAGTAATGCCTTGTGAATGCGCCACAGGGAAGTCTTCGCGTACGCAAACAGCGGATAATACGGTGTCCGCCGGCTCGGCCTACCCCAGCCTTTGATATTAGGAACGGACACCTTGTTGTTTAATAATTCAAAAAACAAAGATCCTACTGAGCCGGTTTAACTCGTCTGTCACCGTGTCGAATACACCCCGACCGAGGGACATCTCAGACCAGAACCTACAACCCTCTTCAACATGGGAAAAGGAACTCGTGTAGGAGAAAACTTGAGGGAGAACTCTATTATAGTCGATCCACTCCGGGATCGTAGTGGGATTCTAACGACCTTTTGGTGACGGATGTAGTGCCTAAGGGTTCCTCTAGCGACGGCCATGGTTACTATCGGTCCTGAACCAGCAGCTTATACTATCCGTATAGTAATCTCTTGACTCTGTGGAGGTGATTTGTGGAAGAAGCTGCCGTCCATAGTTAGTCCCTTTAACCGCACCACTATCTTAAATCATCGACGATCGAGGGTATAACGCGGCGCGAAGCACAACTCGCATCCTAACTGCTTAACTGTTCCCACTTGTAATCGAATGCACGCCAATCGGGAGAGACGCCTATCCGAGTTACACGCCTGTTGGCAGGGGAACCATTACTGATGGCGCAACTGACACCGCCTGCGAACATGAAGCACGGATTAAACAACGTTTGACCCCGTGATTTATGGTCAATAGTACCTATCCCGCACGCGCCATCTCGTTACGATTGACGAGGCCGGCCGACTTTGCGAGCTCCCATTTTACTGCCAAAGTTTGGACGTGCCCGTGGGCCGGGTTAACAGCTCACTACTATGTTCTAGTGGTAGTAAGCAGGCTTGTGCTGCTGGCTGTGTCCCCCCCCCCCCCCCCCCCC",
"TGATAAGGCACATTTCTTAAAATGCAAGCGACGCGGAGGTCGACGAAGTGATGTACGCGACTCCCCTAGAAGGCGCACCTCCAGGCCGCGTGCCACGCCCTCAGGAAAGTATTGACATGAGCATGGTATTTTTTTCGACGGCGTGAAGTGTCCAGCCGTGGACCTACCCTCGCGACGGTCATAAGGGCTACGAGTAATGCTGTGAATGCGCCACAGGGAAGTCGTGCGGTACGCAAACAGCGGATACTACGTGTCCGGCGGCACGTCCTACTCCAGCATTTGGTAATAGAAACGGGACAACCTTGTTGTTTAACTAATTGCAAAAAACACAGATCCTACTGGAGCTGGTTTAAGTCGTCTGTCGCCGTGTCGAATGCACCGGCACCGAGGAAAATTTATGCCCAGAACCTACCACCCTCTATCAACATTGGGGAAAAGGAACTCGTGAGGTGATAACTTGAGGGAGAACTCTATTATAGTCGCTCCACCCTGGGGATCCTAGTGGGAATTCTAACGACCTTTTCGGTGACGGATGTAGTTCCTAAGGGTTTCGCTATCACTGCCATGGTTACTATCTGTCTGAACCACAGCTATATACTTCCGTATGGCAATCCCTTGACTCTGTGGAGGTGATTTGTGGATGATGCTGCCGTCCATTAGTTAGTCCCTTTAATCGCACCACTATCTTATATTAAAGCCGACCAGGGTACAGTAACGCGGTGGAAGCACACCCGCATCCTAGATGCTGAACTGTTCCCACTCTATAATCGAATGTACGCCAATCGGGAGAGACGCCTACCAGTTACTCGCCATGTTGGAGGGGAACCACAGTGATGGGCGCAACTGATCACCGCCTGCGAACATAAGCACGTATCAAACGAACGTTTGTCCCCGTGATTTATGGTCATAGTACCTAGTCCCGCACGCGCCATGTCATTACGGATTGACTAGGCCGGGCCGACTTGGAGCTCGCATTTTAGCTTGCCAAAGTTTGGCCGTGCTTTGCCGTGGTCGTGGCAATAGCACACAAGCCGACGTTCACTGGTAGGTCCCTACTGTAGTGAGGGGGGGGGGGGGGGGGGGG"
))
{
	omajinai();
	struct gaba_score_s const *p = c->params.score_matrix;

	/* fill section */
	struct gaba_section_s const *as = &s->afsec;
	struct gaba_section_s const *bs = &s->bfsec;
	struct gaba_fill_s *f = gaba_dp_fill_root(d, as, 0, bs, 0);
	
	/* fill tail (1) */
	as = (f->status & 0x0f) ? &s->aftail : as;
	bs = (f->status & 0xf0) ? &s->bftail : bs;
	struct gaba_fill_s *t1 = gaba_dp_fill(d, f, as, bs);
	f = (t1->max > f->max) ? t1 : f;

	/* fill tail (2) */
	as = (f->status & 0x0f) ? &s->aftail : as;
	bs = (f->status & 0xf0) ? &s->bftail : bs;
	struct gaba_fill_s *t2 = gaba_dp_fill(d, t1, as, bs);
	f = (t2->max > f->max) ? t2 : f;
	struct gaba_result_s *r = gaba_dp_trace(d, f, NULL, NULL);

	/* naive */
	char const *a = s->seq.a;
	char const *b = s->seq.b;
	struct unittest_naive_result_s n = unittest_naive(p, a, b);

	/* check scores */
	assert(r->score == n.score, "f->max(%lld), r->score(%lld), n.score(%d)",
		f->max, r->score, n.score);
	assert(check_path(r, n.path), "\n%s\n%s",
		format_string_pair_diff(a, b),
		format_string_pair_diff(decode_path(r), n.path));

	debug("score(%lld, %d), alen(%lld), blen(%lld)\n%s",
		r->score, n.score, n.alen, n.blen,
		format_string_pair_diff(decode_path(r), n.path));

	/* cleanup */
	gaba_dp_clean(d);
}
#endif
#endif

/**
 * end of gaba.c
 */