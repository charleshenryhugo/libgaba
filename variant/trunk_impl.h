
/**
 * @file trunk.h
 * @brief macros for trunk (8bit 32cell diff) algorithms
 */
#ifndef _TRUNK_H_INCLUDED
#define _TRUNK_H_INCLUDED

#include "../arch/b8c32.h"
#include "../sea.h"
#include "../util/util.h"
#include <stdint.h>
#include "naive_impl.h"
#include "branch_impl.h"

/**
 * @typedef cell_t
 * @brief cell type in the trunk algorithms
 */
#ifdef cell_t

	#undef cell_t
	#undef CELL_MIN
	#undef CELL_MAX

	#define cell_t 		int8_t
	#define pack_t		uint8_t
	#define CELL_MIN	( INT8_MIN )
	#define CELL_MAX	( INT8_MAX )

#endif

/**
 * @macro BW
 * @brief bandwidth in the trunk algorithm (= 32).
 */
#ifdef BW
#undef BW
#define BW 			( 32 )
#endif

/**
 * @struct trunk_linear_block
 */
struct trunk_linear_block {
	cell_t dp[BLK][BW];
	int64_t i, j;
	/** _vec_acc(accv); */
	int32_t dscu, scc, scl, pacc;
	/** _vec_acc(maxv); */
	int32_t _pad1, max, _pad2, mp;	/** 32bit */
#if DP == DYNAMIC
	_dir_vec(dr);
#endif
};

/**
 * @macro linear_block_t
 */
#ifdef linear_block_t
	#undef linear_block_t
#endif

#define linear_block_t	struct trunk_linear_block

/**
 * @macro bpl, bpb
 * @brief bytes per line, bytes per block
 */
#define trunk_linear_bpl()				( vec_size() )
#define trunk_linear_dp_size()			( BLK * trunk_linear_bpl() )
#define trunk_linear_co_size()			( 2 * sizeof(int64_t) + 2 * vec_acc_size() )
#define trunk_linear_jam_size()			( trunk_linear_co_size() + dr_size() )
#define trunk_linear_head_size()		( trunk_linear_bpl() + trunk_linear_jam_size() + sizeof(struct sea_joint_head) )
#define trunk_linear_tail_size()		( trunk_linear_bpl() + vec_acc_size() + sizeof(struct sea_joint_tail) )
#define trunk_linear_bpb() 				( trunk_linear_dp_size() + trunk_linear_jam_size() )

#define trunk_affine_bpl()				( 2 * vec_size() )
#define trunk_affine_dp_size()			( BLK * trunk_affine_bpl() )
#define trunk_affine_co_size()			( 2 * sizeof(int64_t) + 2 * vec_acc_size() )
#define trunk_affine_jam_size()			( trunk_affine_co_size() + dr_size() )
#define trunk_affine_head_size()		( trunk_affine_bpl() + trunk_affine_jam_size() + sizeof(struct sea_joint_head) )
#define trunk_linear_tail_size()		( trunk_linear_bpl() + vec_acc_size() + sizeof(struct sea_joint_tail) )
#define trunk_affine_bpb() 				( trunk_affine_dp_size() + trunk_affine_jam_size() )

/**
 * @macro (internal) trunk_linear_topq, ...
 * @brief coordinate calculation helper macros
 */
#define trunk_linear_topq(r, k)			naive_linear_topq(r, k)
#define trunk_linear_leftq(r, k)		naive_linear_leftq(r, k)
#define trunk_linear_topleftq(r, k)		naive_linear_topleftq(r, k)
#define trunk_linear_top(r, k) 			naive_linear_top(r, k)
#define trunk_linear_left(r, k)			naive_linear_left(r, k)
#define trunk_linear_topleft(r, k)		naive_linear_topleft(r, k)

#define trunk_affine_topq(r, k)			naive_affine_topq(r, k)
#define trunk_affine_leftq(r, k)		naive_affine_leftq(r, k)
#define trunk_affine_topleftq(r, k)		naive_affine_topleftq(r, k)
#define trunk_affine_top(r, k) 			naive_affine_top(r, k)
#define trunk_affine_left(r, k)			naive_affine_left(r, k)
#define trunk_affine_topleft(r, k)		naive_affine_topleft(r, k)

/**
 * @macro trunk_linear_dir_exp
 * @brief determines the next direction of the lane in the dynamic algorithm.
 */
#if 0
#define trunk_linear_dir_exp_top(r, k, pdp) ( \
	scl += ((dir(r) == TOP \
		? vec_msb(dv) \
		: vec_msb(dh)) + k->gi), \
	scu += ((dir(r) == TOP \
		? vec_lsb(dv) \
		: vec_lsb(dh)) + k->gi), \
	(scl > scu) ? SEA_TOP : SEA_LEFT \
)
#endif
#define trunk_linear_dir_exp_top(r, k, pdp) ( \
	vec_acc_diff(accv) > 0 ? SEA_TOP : SEA_LEFT \
)
#define trunk_linear_dir_exp_bottom(r, k, pdp) 	( 0 )
#define trunk_affine_dir_exp_top(r, k, pdp) 	trunk_linear_dir_exp_top(r, k, pdp)
#define trunk_affine_dir_exp_bottom(r, k, pdp)	trunk_linear_dir_exp_bottom(r, k, pdp)


/**
 * @macro trunk_linear_fill_decl
 */
#define trunk_linear_fill_decl(k, r, pdp) \
	dir_t r; \
	int64_t i, j, p, q; \
	_vec_acc(accv);		/** score accumulator */ \
	_vec_acc(maxv); 	/** max holder */ \
	_vec_single_const(mggv, k->m - 2*k->gi); \
	_vec_single_const(xggv, k->x - 2*k->gi); \
	_vec_char_reg(wq); \
	_vec_char_reg(wt); \
	_vec_cell_reg(dv); \
	_vec_cell_reg(dh); \
	_vec_cell_reg(t1); \
	_vec_cell_reg(t2); \
	_vec_cell_const(zv, 0);

#define trunk_affine_fill_decl(k, r, pdp) \
	trunk_linear_fill_decl(k, r, pdp); \
	_vec_cell_reg(de); \
	_vec_cell_reg(df); \

/**
 * @macro trunk_linear_fill_init
 */
#define trunk_linear_fill_init(k, r, pdp) { \
	/** initialize vectors */ \
	debug("init k(%p), pdp(%p)", k, k->pdp); \
	vec_acc_set(maxv, 0, 0, INT32_MIN, 0); \
	/** load coordinates onto the local stack */ \
	p = _tail(k->pdp, p); \
	i = _tail(k->pdp, i) - DEF_VEC_LEN/2; \
	j = (p - 1) - (i - k->asp) + k->bsp; \
	/** initialize direction array */ \
	debug("init"); \
	dir_init(r, k, k->pdp, p); \
	/** make room for struct sea_joint_head */ \
	pdp += sizeof(struct sea_joint_head); \
	/** load scores of the current vector */ \
	uint8_t *s = _tail(k->pdp, v); \
	debug("init v(%p), dir(%d), d2(%d)", s, dir(r), _tail(k->pdp, d2)); \
	if(_tail(k->pdp, bpc) == 16) { \
		int16_t *t = (int16_t *)s + BW; \
		/** load vectors */ \
		vec_load16_dvdh(s, t, dv, dh, k->gi, _tail(k->pdp, d2)); \
		debug("init acc p(%lld), scl(%d), scc(%d), scu(%d)", p, t[BW-1], t[BW/2], t[0]); \
		/** initialize accumulator */ \
		vec_acc_set(accv, p, t[BW-1], t[BW/2], t[0]); \
	} else {	/** bpc == 4 */ \
		vec_load_dvdh(s, dv, dh); \
		vec_acc_load(s + vec_size(), accv); \
	} \
	debug("init"); \
	vec_store_dvdh(pdp, dv, dh); pdp += vec_size(); \
	/** store the first (i, j) */ \
	*((int64_t *)pdp) = i; pdp += sizeof(int64_t); \
	*((int64_t *)pdp) = j; pdp += sizeof(int64_t); \
	vec_acc_store(pdp, accv); pdp += vec_acc_size(); \
	vec_acc_store(pdp, maxv); pdp += vec_acc_size(); \
	debug("init"); \
	/** write the first dr vector */ \
	dir_end_block(r, k, pdp, p); \
	/** initialize char vectors */ \
	vec_char_setzero(wq); \
	for(q = -BW/2; q < BW/2; q++) { \
		rd_fetch(k->a, i+q); \
		pushq(rd_decode(k->a), wq); \
	} \
	vec_char_setzero(wt); \
	for(q = -BW/2; q < BW/2-1; q++) { \
		rd_fetch(k->b, j+q); \
		pusht(rd_decode(k->b), wt); \
	} \
	debug("init"); \
}

/**
 * @macro trunk_linear_fill_start
 */
#define trunk_linear_fill_start(k, r, pdp) { \
	/** nothing to do */ \
	dir_start_block(r, k, pdp, p); \
}

/**
 * @macro trunk_linear_fill_former_body
 */
#define trunk_linear_fill_former_body(k, r, pdp) { \
	/** nothing to do */ \
}
#define trunk_linear_fill_former_body_cap(k, r, pdp) { \
	/** nothing to do */ \
}

/**
 * @macro trunk_linear_fill_go_down
 */
#define trunk_linear_fill_go_down_intl(k, r, pdp, fetch) { \
	vec_shift_r(dv, dv); \
	fetch(k->b, j+BW/2-1, k->bsp, k->bep, 255); \
	j++; \
	pusht(rd_decode(k->b), wt); \
}
#define trunk_linear_fill_go_down(k, r, pdp) { \
	trunk_linear_fill_go_down_intl(k, r, pdp, rd_fetch_fast); \
}
#define trunk_linear_fill_go_down_cap(k, r, pdp) { \
	trunk_linear_fill_go_down_intl(k, r, pdp, rd_fetch_safe); \
}

/**
 * @macro trunk_linear_fill_go_right
 */
#define trunk_linear_fill_go_right_intl(k, r, pdp, fetch) { \
	vec_shift_l(dh, dh); \
	fetch(k->a, i+BW/2, k->asp, k->aep, 128); \
	i++; \
	pushq(rd_decode(k->a), wq); \
}
#define trunk_linear_fill_go_right(k, r, pdp) { \
	trunk_linear_fill_go_right_intl(k, r, pdp, rd_fetch_fast); \
}
#define trunk_linear_fill_go_right_cap(k, r, pdp) { \
	trunk_linear_fill_go_right_intl(k, r, pdp, rd_fetch_safe); \
}

/**
 * @macro trunk_linear_fill_latter_body
 */
#define trunk_linear_fill_latter_body(k, r, pdp) { \
	vec_comp_sel(t1, wq, wt, mggv, xggv); \
	vec_max(t2, dv, dh); \
	vec_max(t1, t1, t2); \
	vec_sub(t2, t1, dv); \
	vec_sub(dv, t1, dh); \
	vec_assign(dh, t2); \
	vec_store_dvdh(pdp, dv, dh); pdp += vec_size(); \
	if(dir(r) == TOP) { vec_assign(t1, dv); } else { vec_assign(t1, dh); } \
	vec_acc_accum_max(accv, maxv, t1, k->gi); \
	debug("scu(%d), score(%d), scl(%d), p(%lld), (%lld, %lld)", vec_acc_scu(accv), vec_acc_scc(accv), vec_acc_scl(accv), p, i, j); \
	/** caclculate the next advancing direction */ \
	dir_det_next(r, k, pdp, p); 	/** increment p */ \
}
#define trunk_linear_fill_latter_body_cap(k, r, pdp) { \
	trunk_linear_fill_latter_body(k, r, pdp); \
}

/**
 * @macro trunk_linear_fill_empty_body
 */
#define trunk_linear_fill_empty_body(k, r, pdp) { \
	/** increment pdp, and shift direction cache */ \
	vec_store(pdp, zv); pdp += vec_size(); \
	dir_empty(r, k, pdp, p); \
	/*naive_linear_fill_empty_body(k, r, pdp);*/ \
}

/**
 * @macro trunk_linear_fill_end
 */
#define trunk_linear_fill_end(k, r, pdp) { \
	/** store (i, j) to the end of pdp */ \
	*((int64_t *)pdp) = i; pdp += sizeof(int64_t); \
	*((int64_t *)pdp) = j; pdp += sizeof(int64_t); \
	vec_acc_store(pdp, accv); pdp += vec_acc_size(); \
	vec_acc_store(pdp, maxv); pdp += vec_acc_size(); \
	/** store direction vector */ \
	dir_end_block(r, k, pdp, p); \
}

/**
 * @macro trunk_linear_fill_test_xdrop
 */
#define trunk_linear_fill_test_xdrop(k, r, pdp) ( \
	  ((int64_t)XSEA - k->alg - 1) \
	& ((int64_t)vec_acc_scc(accv) + k->tx - vec_acc_scc(maxv)) \
)
#define trunk_linear_fill_test_xdrop_cap(k, r, pdp) ( \
	trunk_linear_fill_test_xdrop(k, r, pdp) \
)

/**
 * @macro trunk_linear_fill_test_bound
 */
#define trunk_linear_fill_test_bound(k, r, pdp) ( \
	naive_linear_fill_test_bound(k, r, pdp) \
)
#define trunk_linear_fill_test_bound_cap(k, r, pdp) ( \
	naive_linear_fill_test_bound_cap(k, r, pdp) \
)

/**
 * @macro trunk_linear_fill_test_mem
 */
#define trunk_linear_fill_test_mem(k, r, pdp) ( \
	(int64_t)(k->tdp - pdp \
		- (3*trunk_linear_bpb() \
		+ sizeof(struct sea_joint_tail) \
		+ sizeof(struct sea_joint_head) \
		+ 2 * trunk_linear_bpl()))		/** v + accv */ \
)
#define trunk_linear_fill_test_mem_cap(k, r, pdp) ( \
	(int64_t)(k->tdp - pdp \
		- (trunk_linear_bpb() \
		+ sizeof(struct sea_joint_tail) \
		+ sizeof(struct sea_joint_head) \
		+ 2 * trunk_linear_bpl()))		/** v + accv */ \
)

/**
 * @macro trunk_linear_fill_test_chain
 */
#define trunk_linear_fill_test_chain(k, r, pdp)		( 0 )	/** never chain */
#define trunk_linear_fill_test_chain_cap(k, r, pdp)	( 0 )

/**
 * @macro trunk_linear_fill_check_term
 */
#define trunk_linear_fill_check_term(k, r, pdp) ( \
	( trunk_linear_fill_test_xdrop(k, r, pdp) \
	| trunk_linear_fill_test_bound(k, r, pdp) \
	| trunk_linear_fill_test_mem(k, r, pdp) \
	| trunk_linear_fill_test_chain(k, r, pdp)) < 0 \
)
#define trunk_linear_fill_check_term_cap(k, r, pdp) ( \
	( trunk_linear_fill_test_xdrop_cap(k, r, pdp) \
	| trunk_linear_fill_test_bound_cap(k, r, pdp) \
	| trunk_linear_fill_test_mem_cap(k, r, pdp) \
	| trunk_linear_fill_test_chain_cap(k, r, pdp)) < 0 \
)

/**
 * @macro trunk_linear_fill_search_advance_ptr
 */
#define trunk_linear_fill_search_advance_ptr(k, r, pdp) { \
}

#if 0

	do { \
		m = *--pmk; p--; \
		if(((p - sp) & (BLK-1)) == 0) { \
			pln -= bpb(); \
			dir_jump_backward(r, k, pdp, p, sp); \
		} \
		debug("p(%lld), m(%u)", p, m & mask); \
	} while((m & mask) == 0); \


	/** here b and pbk points to the next block of the last searched one */ \
	do { \
		/** load the next direction pointer */ \
		dir_load_forward(r, k, pdp, p, sp); \
		/** update pln (uint8_t *) */ \
		if(((p - sp) & (BLK-1)) == 0) { pln += jam_size(); } \
		pln += bpl(); \
		/** load, add, and take max */ \
		vec_load_dvdh(pln, dv, dh); \
		if(dir(r) == TOP) { vec_add(t1, t1, dv); } \
		else { vec_add(t1, t1, dh); } \
		vec_add(t1, t1, gv); \
		vec_max(t2, t2, t1); \
		vec_comp_mask(m, t1, t2); \
		debug("p(%lld), m(%u)", p, m); \
		vec_print(stdout, t1); \
		vec_print(stdout, t2); \
		*pmk++ = m; 	/** save mask */ \
	} while(pln < pbe); \


		m = *--pmk; \
		while((m & mask) == 0) { \
			debug("p(%lld), m(%u)", p, m & mask); \
			dir_go_backward(r, k, pdp, p, sp); \
			if(((p - sp) & (BLK-1)) == 0) { \
				pco -= bpb(); \
				/*dir_jump_backward(r, k, pdp, p, sp);*/ \
			} \
			m = *--pmk; p--; \
		} \


#endif

#define trunk_linear_fill_search_blk(k, r, pdp, b, bn) { \
	uint32_t m;							/** mask */ \
	_vec_cell_const(gv, k->gi); \
	/** here pmx points to the head of the max block */ \
	/*p = MAX2(sp, bp - BLK) - 1;*/			/** the last vector of the previous block */ \
	/*uint8_t *pbs = MAX2(k->pdp + head_size(), (pmx) - bpb()) - (jam_size() + bpl());*/ \
	/*uint8_t *pbe = MIN2(pdp - tail_size(), (pmx) + 2*bpb()) - (jam_size() + bpl());*/ \
	uint32_t *pmk = (uint32_t *)pdp;	/** tail of the section, as mask temporary */ \
	/*debug("pbs(%p), pbe(%p), pmk(%p), k->pdp(%p), pdp(%p)", pbs, pbe, pmk, k->pdp, pdp);*/ \
	int64_t sb = MAX2(b - 1, 0); \
	int64_t eb = MIN2(b + 2, bn + 1); \
	linear_block_t *pbk = (linear_block_t *)(k->pdp + head_size()) + sb; \
	linear_block_t *pbs = pbk-1;		/** base block */ \
	/** initialize accumulator */ \
	vec_setzero(t1);					/** temporary 1, declared in fill_decl */ \
	vec_setzero(t2);					/** temporary 2 as max vector */ \
	/** initialize direction pointer */ \
	/*uint8_t *pln = pbs;*/ \
	p = sb*BLK+sp; \
	dir_set_pdr(r, k, k->pdp, p, sp); \
	/** accumulate */ \
	int64_t a, l; \
	for(a = sb; a < eb; a++, pbk++) { \
		for(l = 0; l < BLK; l++) { \
			vec_load_dvdh(pbk->dp[l], dv, dh); \
			if(dir(r) == TOP) { vec_add(t1, t1, dv); } \
			else { vec_add(t1, t1, dh); } \
			vec_add(t1, t1, gv); \
			vec_max(t2, t2, t1); \
			vec_comp_mask(m, t1, t2); \
			debug("p(%lld), m(%u)", p, m); \
			vec_print(t1); \
			vec_print(t2); \
			*pmk++ = m; 	/** save mask */ \
			/** load the next direction */ \
			dir_load_forward(r, k, pdp, p, sp); \
		} \
	} \
	/** detect the lane */ \
	/** load the base vector */ \
	int32_t pos, val; \
	vec_load_dvdh(pbs->dp[BLK-1], dv, dh); \
	vec_maxpos_dvdh(pos, val, dv, dh, t2);		/** prefixsum & maxpos */ \
	/*int32_t *psc = (int32_t *)(pbs + bpl() + 2 * sizeof(int64_t));*/ \
	/*max = psc[2] - psc[0] + val;*/		/** v[31] - (v[31] - v[0]) + diff */ \
	max = pbs->scl - pbs->dscu + val; \
	debug("max(%d), val(%d), pos(%d), p(%lld)", max, val, pos, p); \
	if(max >= k->max) { \
		/** search breakpoint */ \
		uint32_t mask = 0x01<<pos; \
		/*uint8_t *pco = pbe + bpl();*/ \
		for(b = (eb-sb)*BLK; b > 0; b--) { \
			debug("b(%lld), p(%lld), m(%u)", b, p, *(pmk-1)); \
			dir_go_backward(r, k, pdp, p, sp); \
			if((*--pmk & mask) != 0) { break; } \
		} \
		debug("p(%lld), m(%u), b(%lld)", p, m & mask, 1+b-sb); \
		k->mp = p; \
		k->mq = pos - BW/2; \
		debug("i(%lld), di(%lld)", (pbs + b/BLK)->i, dir_sum_i_blk(r, k, pdp, p, sp)); \
		k->mi = (pbs + b/BLK)->i + dir_sum_i_blk(r, k, pdp, p, sp) - (pos - BW/2); \
		/*k->mi = (pbs + b/BLK + 1)->i - dir_sum_i_blk(r, k, pdp, p, sp) - (pos - BW/2);*/ \
		/*k->mi = *((int64_t *)pco) - dir_sum_i_blk(r, k, pdp, p, sp);*/ \
		k->max = max; \
	} \
	debug("p(%lld), q(%lld), i(%lld)", k->mp, k->mq, k->mi); \
}

/**
 * @macro trunk_linear_fill_search_max
 */
#define trunk_linear_fill_search_max(k, r, pdp) { \
	/** here pdp and p points to the tail of the section */ \
	/*int64_t ofs = dp_size() + co_size() - 3*sizeof(int32_t);*/ \
	/*uint8_t *pmx = pdp - tail_size() - bpb() + ofs;*/	/** max of the last block */ \
	/** load corresponding coordinates */ \
	/** load p-coordinate */ \
	debug("size(%lu), size(%lu)", bpb(), sizeof(linear_block_t)); \
	int64_t sp = _tail(k->pdp, p); \
	/** calculate block number and base address */ \
	int64_t bn = blk_num((p-1) - sp, 0); \
	linear_block_t *pbk = (linear_block_t *)(k->pdp + head_size()) + bn; \
	debug("start search max sp(%lld), bn(%lld), p(%lld)", sp, bn, p); \
	/** search a breakpoint */ \
	int64_t b; \
	for(b = bn; b > 0; b--, pbk--) { \
		/*debug("max(%d), max(%d)", max, *((int32_t *)(pmx - bpb())));*/ \
		/*if(*((int32_t *)(pmx - bpb())) != max) { break; }*/ \
		debug("max(%d), max(%d)", max, (pbk - 1)->max); \
		debug("%d, %d, %d, %d, %d, %d, %d, %d", \
			(pbk - 1)->dscu, (pbk - 1)->scc, (pbk - 1)->scl, (pbk - 1)->pacc, \
			(pbk - 1)->_pad1, (pbk - 1)->max, (pbk - 1)->_pad2, (pbk - 1)->mp); \
		if((pbk - 1)->max != max) { break; } \
	} \
	debug("block determined b(%lld), bp(%lld)", b, b*BLK+sp); \
	/** determine search area */ \
	trunk_linear_fill_search_blk(k, r, pdp, b, bn); \
}

/**
 * @macro trunk_linear_fill_finish
 */
#define trunk_linear_fill_finish(k, r, pdp) { \
	/** save vectors */ \
	uint8_t *v = pdp; \
	vec_store_dvdh(pdp, dv, dh); pdp += vec_size(); \
 	vec_acc_store(pdp, accv); pdp += vec_acc_size(); \
	/** create ivec at the end */ \
	pdp += sizeof(struct sea_joint_tail); \
	/** save terminal coordinates */ \
	debug("p(%lld), i(%lld), max(%d)", p, i, vec_acc_scc(maxv)); \
	_tail(pdp, p) = p; \
	_tail(pdp, i) = i + DEF_VEC_LEN/2; \
	_tail(pdp, v) = v; \
	_tail(pdp, bpc) = 4; \
	_tail(pdp, d2) = dir_raw(r); \
	/** load max of the section on center */ \
	int32_t max = vec_acc_scc(maxv); \
	debug("max(%d)", max); \
	/** search max */ \
	if(k->alg != NW && (max + 16*k->m) > k->max) { \
		/** search */ \
		trunk_linear_fill_search_max(k, r, pdp); \
	} \
	/** save p-coordinate at the beginning of the block */ \
	/*_head(k->pdp, max) = vec_acc_scc(max);*/ \
}

#if 0
/**
 * @macro trunk_linear_chain_save_len
 */
#define trunk_linear_chain_save_len(t, c, k)		( 3 * BW )

/**
 * @macro trunk_linear_chain_push_tail
 *
 * absolute valueへの変換をSIMDを使って高速にしたい。
 * extract -> scatter -> addのループを32回回す。
 * prefix sumなので、log(32) = 5回のループでできないか。
 */
#define trunk_linear_chain_push_tail(t, c, k) { \
	int16_t psc, csc; \
	cell_t *p = (cell_t *)c.pdp - 3*BW; \
	t.i += BW/2; \
	t.j -= BW/2; \
	psc = -k->gi - *(p + BW) - *p; \
	if(c.pdr[t.p] == TOP) { \
		for(t.q = 0; t.q < BW; t.q++) { \
			*((int16_t *)c.pdp) = psc; \
			psc += *p; \
			csc = psc + *(p + BW) + k->gi; \
			*((int16_t *)c.pdp + BW) = csc]][]; \
			debug("psc(%d), csc(%d)", psc, csc); \
			p++; \
			c.pdp += sizeof(int16_t); \
		} \
	} else { \
		for(t.q = 0; t.q < BW; t.q++) { \
			psc += *p; \
			*((int16_t *)c.pdp) = psc; \
			csc = psc + *(p + BW) + k->gi; \
			*((int16_t *)c.pdp + BW) = csc; \
			debug("psc(%d), csc(%d)", psc, csc); \
			p++; \
			c.pdp += sizeof(int16_t); \
		} \
	} \
	c.pdp += sizeof(int16_t) * BW; \
	c.v.size = sizeof(int16_t); \
	c.v.plen = c.v.clen = BW; \
	c.v.pv = c.pdp - sizeof(int16_t) * 2*BW; \
	c.v.cv = c.pdp - sizeof(int16_t) * BW; \
	debug("ivec: dir(%d)", c.pdr[t.p]); \
}
#endif

/**
 * @macro trunk_linear_set_terminal
 */
#define trunk_linear_set_terminal(k, pdp) { \
	naive_linear_set_terminal(k, pdp); \
}
#if 0
	dir_t r; \
	cell_t *psc = pdp + addr(t.p - sp, 0); \
	int64_t sc = pdp[addr(t.p - sp + 1, -BW/2 + 1)]; /** score */ \
	t.mi = c.aep; \
	t.mj = c.bep; \
	t.mp = cop(t.mi, t.mj, BW) - cop(c.asp, c.bsp, BW); \
	t.mq = coq(t.mi, t.mj, BW) - coq(t.i, t.j, BW); /** COP(mi, mj) == COP(i, j)でなければならない */ \
	dir_term(r, k); \
	while(t.p > t.mp) { \
		dir_prev(r, t, c); \
		sc -= (dir(r) == TOP) ? DV(psc, k->gi) : DH(psc, k->gi); \
		psc -= trunk_linear_bpl(c); \
	} \
	while(t.q < t.mq) { \
		sc += (DV(psc+1, k->gi) - DH(psc, k->gi)); \
		psc++; t.q++; \
	} \
	while(t.q > t.mq) { \
		sc += (DH(psc-1, k->gi) - DV(psc, k->gi)); \
		psc--; t.q--; \
	} \
}
#endif

#if 0
/**
 * @macro trunk_linear_search_trigger
 */
#define trunk_linear_search_trigger(t1, t2, c, k) ( \
	t1.max > (t2.max - (k->m - 2*k->gi)*BW/2 - (t1.max > t2.max)) \
)

/**
 * @macro trunk_linear_search_max_score
 */
/*
#define trunk_linear_search_max_score(t, c, k) { \
	t.mi = t.i; t.mj = t.j; t.mp = t.p; t.mq = 0; \
}
*/
#define trunk_linear_search_max_score(t, c, k) { \
	dir_t r; \
	int64_t msp = MAX2(t.mp + BW * (k->m - 2*k->gi) / (2 * k->x), sp); \
	int64_t mep = MIN2(t.mp + BW * (k->m - 2*k->gi) / (2 * k->m), t.p); \
	int32_t sc = 0, tsc; \
	struct sea_coords x, y; \
	_vec_cell_reg(dv); \
	_vec_cell_reg(dh); \
	\
	debug("msp(%lld), mep(%lld)", msp, mep); \
	debug("mi(%lld), mj(%lld), mp(%lld), mq(%lld), max(%d)", t.mi, t.mj, t.mp, t.mq, t.max); \
	memset(&x, 0, sizeof(struct sea_coords)); \
	memset(&y, 0, sizeof(struct sea_coords)); \
	c.pdp = (uint8_t *)(pb + ADDR(msp - sp, -BW/2, BW)); \
	dir_init(r, c.pdr[msp]); \
	vec_load_dvdh(c.pdp, dv, dh); \
	for(x.p = msp; x.p < mep;) { \
		dir_next_guided(r, x, c); \
		vec_load_dvdh(c.pdp, dv, dh); \
		debug("load: pdp(%p)", c.pdp); \
		sc += (((dir(r) == TOP) ? vec_lsb(dv) : vec_lsb(dh)) + k->gi); \
		if(dir(r) == TOP) { x.j++; } else { x.i++; } \
		debug("i(%lld), j(%lld), p(%lld), q(%lld), sc(%d)", x.i, x.j, x.p, x.q, sc); \
		for(x.q = -BW/2, tsc = sc; x.q < BW/2; x.q++) { \
			if(tsc >= x.max) { \
				x.max = tsc; \
				coord_save_m(x); \
				debug("max found: i(%lld), j(%lld), p(%lld), q(%lld), sc(%d)", x.i, x.j, x.p, x.q, x.max); \
			} \
			if(x.q == 0 && x.p == t.mp) { \
				y = x; y.max = tsc; \
				debug("anchor found: i(%lld), j(%lld), p(%lld), q(%lld), sc(%d), max(%d)", x.i, x.j, x.p, x.q, tsc, x.max); \
			} \
			tsc -= (vec_lsb(dh) + k->gi); \
			vec_shift_r(dh, dh); \
			vec_shift_r(dv, dv); \
			tsc += (vec_lsb(dv) + k->gi); \
		} \
	} \
	debug("t: mi(%lld), mj(%lld), mp(%lld), mq(%lld), max(%d)", t.mi, t.mj, t.mp, t.mq, t.max); \
	debug("x: mi(%lld), mj(%lld), mp(%lld), mq(%lld), max(%d)", x.mi, x.mj, x.mp, x.mq, x.max); \
	debug("y: mi(%lld), mj(%lld), mp(%lld), mq(%lld), max(%d)", y.mi, y.mj, y.mp, y.mq, y.max); \
	t.max += (x.max - y.max); \
	t.mi += (x.mi - y.i - x.mq); \
	t.mj += (x.mj - y.j + x.mq); \
	t.mp = x.mp; \
	t.mq = x.mq; \
	debug("mi(%lld), mj(%lld), mp(%lld), mq(%lld), max(%d)", t.mi, t.mj, t.mp, t.mq, t.max); \
}

#if 0
#define trunk_linear_search_max_score(t, c, k) { \
	/*if(t.max > t.max - (k->m - 2*k->gi)*BW/2) {*/ \
	dir_t r; \
	cell_t *psc = pb + ADDR(t.p - sp, 0, BW); \
	t.mp = MAX2(t.mp + BW * (k->m - 2*k->gi) / (2 * k->x), sp); \
	t.mq = 0; \
	dir_term(r, t, c); \
	while(t.p > t.mp) { \
		dir_prev(r, t, c); \
		t.max -= (dir(r) == TOP) ? DV(psc, k->gi) : DH(psc, k->gi); \
		if(dir(r) == TOP) { t.j--; } else { t.i--; } \
		psc -= trunk_linear_bpl(c); \
	} \
	/** ここで(i, j)はsearch開始座標 / (mi, mj)は0にセットする */ \
	{ \
		int64_t scu = 0, score = 0, scl = 0; \
		t.max = 0; \
		_vec_cell_reg(dv); \
		_vec_cell_reg(dh); \
		_vec_cell_reg(tmp); \
		vec_load_dvdh(c.pdp, dv, dh); \
		trunk_linear_fill_finish(k, r, pdp); \
		trunk_linear_chain_push_tail(t, c, k); \
	} \
	{ \
		struct sea_coords b = t; \
		k.f->branch(&k, &c, &b); \
		t.max += b.max; \
		t.mp += b.mp; t.mq += b.mq; \
	} \
}
#endif
#endif

/**
 * @macro trunk_linear_trace_decl
 */
#define trunk_linear_trace_decl(k, r, pdp) \
	dir_t r; \
	cell_t *pdg, *pvh;		/** p-1, p */ \
	int64_t i, j, p, q, sp;

/**
 * @macro trunk_linear_trace_windback_ptr
 */
#define trunk_linear_trace_windback_ptr(k, r, pdp) { \
	/** load the next direction pointer */ \
	dir_load_backward(r, k, pdp, p, sp); \
	/** update pdg, pvh, and ptb (cell_t *) before loading the next direction */ \
	pvh = pdg; pdg -= bpl() / sizeof(cell_t); \
	if(((p - sp) & (BLK-1)) == 0) { \
		/*dir_jump_backward(r, k, pdp, p, sp);*/ \
		pdg -= jam_size() / sizeof(cell_t); \
	} \
}

/**
 * @macro trunk_linear_trace_init
 *
 * push_tailの実装を使って、absolute scoreに変換する。
 * ここからnon-diffの計算をし、maxの場所を特定する。
 */
#define trunk_linear_trace_init(k, r, pdp) { \
	/** load coordinates */ \
	debug("init trace"); \
	p = _head(k->pdp, p); \
	q = _head(k->pdp, q); \
	i = _head(k->pdp, i); \
	j = p - (i - k->asp) + k->bsp; \
	sp = _tail(pdp, p); \
	/** initialize pointers */ \
	pvh = (cell_t *)(pdp + addr(p - sp, 0)); \
	pdg = (cell_t *)(pdp + addr((p - 1) - sp, 0)); \
	dir_set_pdr(r, k, pdp, p, sp); \
	/** fetch the last characters */ \
	rd_fetch(k->a, i-1); \
	rd_fetch(k->b, j-1); \
}

#if 0

	for(int a = i-20; a < i+20; a++) { putchar(k->a.p[a]); } \
	printf("\n"); \
	for(int a = j-20; a < j+20; a++) { putchar(k->b.p[a]); } \
	printf("\n"); \
	for(int a = -20; a < 20; a++) { putchar(a == -1 ? '^' : ' '); } \
	printf("\n"); \

#endif

/**
 * @macro trunk_linear_trace_body
 */
#define trunk_linear_trace_body(k, r, pdp) { \
	/** calculate diagonal difference */ \
	debug("dir: d(%d), d2(%d)", dir(r), dir2(r)); \
	cell_t dh = DH(pvh + q, k->gi); \
	cell_t diag = dh + DV(pdg + q + trunk_linear_leftq(r, k), k->gi); \
	cell_t sc = rd_cmp(k->a, k->b) ? k->m : k->x; \
	debug("traceback: (%lld, %lld), (%lld, %lld), diag(%d), sc(%d), dh(%d), dv(%d), dh-1(%d), dv-1(%d), left(%d), top(%d)", \
		p, q, i, j, \
		diag, sc, \
		DH(pvh, k->gi), DV(pvh, k->gi), \
		DH(pdg + trunk_linear_topq(r, k), k->gi), \
		DV(pdg + trunk_linear_leftq(r, k), k->gi), \
		trunk_linear_leftq(r, k), trunk_linear_topq(r, k)); \
	if(sc == diag) { \
		q += trunk_linear_topleftq(r, k); \
		trunk_linear_trace_windback_ptr(k, r, pdp); \
		i--; rd_fetch(k->a, i-1); \
		j--; rd_fetch(k->b, j-1); \
		if(sc == k->m) { wr_pushm(k->l); } else { wr_pushx(k->l); } \
	} else if(dh == k->gi) { \
		q += trunk_linear_leftq(r, k); \
		i--; rd_fetch(k->a, i-1); \
		debug("del"); \
		wr_pushd(k->l); \
	} else if(DV(pvh + q, k->gi) == k->gi) { \
		q += trunk_linear_topq(r, k); \
		j--; rd_fetch(k->b, j-1); \
		debug("ins"); \
		wr_pushi(k->l); \
	} else { \
		debug("out of band"); \
		return SEA_ERROR_OUT_OF_BAND; \
	} \
	if(q < -BW/2 || q > BW/2-1) { \
		debug("out of band t.mq(%lld)", q); \
		return SEA_ERROR_OUT_OF_BAND; \
	} \
	/** windback to p-1 */ \
	trunk_linear_trace_windback_ptr(k, r, pdp); \
}

/**
 * @macro trunk_linear_trace_test_bound
 */
#define trunk_linear_trace_test_bound(k, r, pdp) ( \
	p - sp \
)

/**
 * @macro trunk_linear_trace_test_sw
 */
#define trunk_linear_trace_test_sw(k, r, pdp) ( \
	0 /*((k->alg == SW) ? -1 : 0) & (cc - 1)*/ \
)

/**
 * @macro trunk_linear_trace_check_term
 */
#define trunk_linear_trace_check_term(k, r, pdp) ( \
	( trunk_linear_trace_test_bound(k, r, pdp) \
	| trunk_linear_trace_test_sw(k, r, pdp)) < 0 \
)

/**
 * @macro trunk_linear_trace_finish
 */
#define trunk_linear_trace_finish(k, r, pdp) { \
	_head(pdp, p) = p; \
	_head(pdp, q) = q; \
	_head(pdp, i) = i; \
	/*_head(pdp, score) = cc;*/ \
}

#endif /* #ifndef _TRUNK_H_INCLUDED */
/**
 * end of trunk.h
 */
