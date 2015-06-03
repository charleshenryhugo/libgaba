
/**
 * @file sea.c
 *
 * @brief a top level implementation of the libsea C API
 *
 * @author Hajime Suzuki
 * @date 12/29/2014
 * @license Apache v2
 */

#include <stdlib.h>				/* for malloc / free */
#include <string.h>				/* memset */
#include "include/sea.h"		/* definitions of public APIs and structures */
#include "util/util.h"			/* definitions of internal utility functions */
// #include "build/config.h"

#if HAVE_ALLOCA_H
 	#include <alloca.h>
#endif

int32_t
(*func_table[3][3][7])(
	struct sea_context const *ctx,
	struct sea_process *proc) = {
	{
		{NULL, NULL, NULL, NULL, NULL, NULL, NULL},
		{NULL, NULL, NULL, NULL, NULL, NULL, NULL},
		{NULL, NULL, NULL, NULL, NULL, NULL, NULL}
	},
	{
		{NULL, NULL, NULL, NULL, NULL, NULL},
		{naive_linear_dynamic, twig_linear_dynamic, branch_linear_dynamic, trunk_linear_dynamic, balloon_linear_dynamic, balloon_linear_dynamic, cap_linear_dynamic},
		{naive_linear_guided, twig_linear_guided, branch_linear_guided, trunk_linear_guided, balloon_linear_guided, balloon_linear_guided, cap_linear_guided}
	},
	{
		{NULL, NULL, NULL, NULL, NULL, NULL},
		{naive_affine_dynamic, twig_affine_dynamic, branch_affine_dynamic, trunk_affine_dynamic, balloon_affine_dynamic, balloon_affine_dynamic, cap_affine_dynamic},
		{naive_affine_guided, twig_affine_guided, branch_affine_guided, trunk_affine_guided, balloon_affine_guided, balloon_affine_guided, cap_affine_guided}
	}
};

uint8_t
(*rd_table[8])(
	uint8_t const *p,
	int64_t pos) = {
	NULL,
	_pop_ascii,
	_pop_4bit,
	_pop_2bit,
	_pop_4bit8packed,
	_pop_2bit8packed,
	NULL
};

int64_t
(*wr_table[4][2][6])(
	uint8_t *p,
	int64_t pos) = {
	{
		{NULL, NULL, NULL, NULL, NULL, NULL},
		{NULL, NULL, NULL, NULL, NULL, NULL}
	},
	{
		{_init_ascii_f, _pushm_ascii_f, _pushx_ascii_f, _pushi_ascii_f, _pushd_ascii_f, _finish_ascii_f},
		{_init_ascii_r, _pushm_ascii_r, _pushx_ascii_r, _pushi_ascii_r, _pushd_ascii_r, _finish_ascii_r}
	},
	{
		{_init_cigar_f, _pushm_cigar_f, _pushx_cigar_f, _pushi_cigar_f, _pushd_cigar_f, _finish_cigar_f},
		{_init_cigar_r, _pushm_cigar_r, _pushx_cigar_r, _pushi_cigar_r, _pushd_cigar_r, _finish_cigar_r}
	},
	{
		{_init_dir_f, _pushm_dir_f, _pushx_dir_f, _pushi_dir_f, _pushd_dir_f, _finish_dir_f},
		{_init_dir_r, _pushm_dir_r, _pushx_dir_r, _pushi_dir_r, _pushd_dir_r, _finish_dir_r}
	}
};

/**
 * @fn sea_aligned_malloc
 *
 * @brief an wrapper of posix_memalign
 *
 * @param[in] size : size of memory in bytes.
 * @param[in] align : alignment size.
 *
 * @return a pointer to the allocated memory.
 */
void *sea_aligned_malloc(size_t size, size_t align)
{
	void *ptr;
	posix_memalign(&ptr, align, size);
	return(ptr);
}

/**
 * @fn sea_aligned_free
 *
 * @brief free memory which is allocated by sea_aligned_malloc
 *
 * @param[in] ptr : a pointer to the memory to be freed.
 */
void sea_aligned_free(void *ptr)
{
	free(ptr);
	return;
}

/**
 * @macro AMALLOC
 *
 * @brief an wrapper macro of alloca or sea_aligned_malloc
 */
#define ALLOCA_THRESH_SIZE		( 1000000 )		/** 1MB */

#if HAVE_ALLOCA_H
	#define AMALLOC(ptr, size, align) { \
	 	if((size) > ALLOCA_THRESH_SIZE) { \
	 		(ptr) = sea_aligned_malloc(size, align); \
	 	} else { \
	 		(ptr) = alloca(size); (ptr) = (((ptr)+(align)-1) / (align)) * (align); \
	 	} \
	}
#else
	#define AMALLOC(ptr, size, align) { \
		(ptr) = sea_aligned_malloc(size, align); \
	}
#endif /* #if HAVE_ALLOCA_H */

/**
 * @macro AFREE
 *
 * @breif an wrapper macro of alloca or sea_aligned_malloc
 */
#if HAVE_ALLOCA_H
	#define AFREE(ptr, size) { \
	 	if((size) > ALLOCA_THRESH_SIZE) { \
	 		sea_aligned_free(ptr); \
	 	} \
	}
#else
	#define AFREE(ptr, size) { \
		sea_aligned_free(ptr); \
	}
#endif /* #if HAVE_ALLOCA_H */

/**
 * @fn sea_init_flags_vals
 *
 * @brief (internal) check arguments and fill proper values (or default values) to sea_context.
 *
 * @param[ref] ctx : a pointer to blank context.
 * @param[in] flags : option flags. see flags.h for more details.
 * @param[in] len : maximal alignment length. give 0 if you are not sure. (len >= 0)
 * @param[in] m : match award in the Dynamic Programming. (m >= 0)
 * @param[in] x :  mismatch cost. (x < m)
 * @param[in] gi : gap open cost. (or just gap cost in the linear-gap cost) (2gi <= x)
 * @param[in] ge : gap extension cost. (ge <= 0) valid only in the affine-gap cost. the total penalty of the gap with length L is gi + (L - 1)*ge.
 * @param[in] xdrop : xdrop threshold. (xdrop > 0) valid only in the seed-and-extend alignment. the extension is terminated when the score S meets S < max - xdrop.
 *
 * @return SEA_SUCCESS if proper values are set, corresponding error number if failed.
 *
 * @sa sea_init, sea_init_fp
 */
static int32_t
sea_init_flags_vals(
	sea_t *ctx,
	int32_t flags,
	int8_t m,
	int8_t x,
	int8_t gi,
	int8_t ge,
	int32_t tx,
	int32_t tc,
	int32_t tb)
{
	int32_t int_flags = flags;		/* internal copy of the flags */
	int32_t error_label = SEA_ERROR;

	if((int_flags & SEA_FLAGS_MASK_ALG) == 0) {
		return SEA_ERROR_UNSUPPORTED_ALG;
	}

	/** default: affine-gap cost */
	if((int_flags & SEA_FLAGS_MASK_COST) == 0) {
		int_flags = (int_flags & ~SEA_FLAGS_MASK_COST) | SEA_AFFINE_GAP_COST;
	}

	/** default: banded DP */
	if((int_flags & SEA_FLAGS_MASK_DP) == 0) {
		int_flags = (int_flags & ~SEA_FLAGS_MASK_DP) | SEA_DYNAMIC;
	}

	/** default input sequence format: ASCII */
	if((int_flags & SEA_FLAGS_MASK_SEQ_A) == 0) {
		int_flags = (int_flags & ~SEA_FLAGS_MASK_SEQ_A) | SEA_SEQ_A_ASCII;
	}
	if((int_flags & SEA_FLAGS_MASK_SEQ_B) == 0) {
		int_flags = (int_flags & ~SEA_FLAGS_MASK_SEQ_B) | SEA_SEQ_B_ASCII;
	}

	/** default output format: direction string.
	 * (yields RDRDRDRD for the input pair (AAAA and AAAA).
	 * use SEA_ALN_CIGAR for cigar string.)
	 */
	if((int_flags & SEA_FLAGS_MASK_ALN) == 0) {
		int_flags = (int_flags & ~SEA_FLAGS_MASK_ALN) | SEA_ALN_ASCII;
	}

	/** check if DP cost values are proper. the cost values must satisfy m >= 0, x < m, 2*gi <= x, ge <= 0. */
	if(m < 0 || x >= m || 2*gi > x || ge > 0) {
		return SEA_ERROR_INVALID_COST;
	}

	/** if linear-gap option is specified, set unused value (gap extension cost) zero. */
	if((int_flags & SEA_FLAGS_MASK_COST) == SEA_LINEAR_GAP_COST) {
		ge = 0;
	}
	
	/* push scores to context */
	ctx->m = m;
	ctx->x = x;
	ctx->gi = gi;
	ctx->ge = ge;

	/* check if thresholds are proper */
	if(tx < 0 || tc < 0 || tb < 0) {
		return SEA_ERROR_INVALID_ARGS;
	}
	ctx->tx = tx;
	ctx->tc = tc;
	ctx->tb = tb;

	ctx->bw = 32;			/** fixed!! */
	if((int_flags & SEA_FLAGS_MASK_ALG) == SEA_SW) {
		ctx->min = 0;
	} else {
		ctx->min = INT32_MIN + 10;
	}
	ctx->alg = int_flags & SEA_FLAGS_MASK_ALG;

	/* special parameters */
	ctx->mask = 0;			/** for the mask-match algorithm */
	ctx->k = 4;				/** search length stretch ratio: default is 4 */
	ctx->isize = ALLOCA_THRESH_SIZE;	/** inital matsize = 1M */
	ctx->memaln = 256;	/** (MAGIC) memory alignment size */

	/* push flags to the context */
	ctx->flags = int_flags;

	error_label = SEA_SUCCESS;
	return(error_label);
}

/**
 * @fn sea_init
 *
 * @brief constructs and initializes an alignment context.
 *
 * @param[in] flags : option flags. see flags.h for more details.
 * @param[in] len : maximal alignment length. len must hold len > 0.
 * @param[in] m : match award in the Dynamic Programming. (m >= 0)
 * @param[in] x :  mismatch cost. (x < m)
 * @param[in] gi : gap open cost. (or just gap cost in the linear-gap cost) (2gi <= x)
 * @param[in] ge : gap extension cost. (ge <= 0) valid only in the affine-gap cost. the total penalty of the gap with length L is gi + (L - 1)*ge.
 * @param[in] xdrop : xdrop threshold. (xdrop > 0) valid only in the seed-and-extend alignment. the extension is terminated when the score S meets S < max - xdrop.
 *
 * @return a pointer to sea_context structure.
 *
 * @sa sea_free, sea_sea
 */
sea_t *sea_init(
	int32_t flags,
	int8_t m,
	int8_t x,
	int8_t gi,
	int8_t ge,
	int32_t tx,
	int32_t tc,
	int32_t tb)
{
	int32_t i;
	sea_t *ctx = NULL;
	int32_t error_label = SEA_ERROR;


	/** malloc sea_context */
	ctx = (sea_t *)malloc(sizeof(struct sea_context));
	if(ctx == NULL) {
		error_label = SEA_ERROR_OUT_OF_MEM;
		goto _sea_init_error_handler;
	}

	/** init value fields of sea_context */
	if((error_label = sea_init_flags_vals(ctx, flags, m, x, gi, ge, tx, tc, tb)) != SEA_SUCCESS) {
		goto _sea_init_error_handler;
	}

	/* check if error_label is success */
	if(error_label != SEA_SUCCESS) {
		goto _sea_init_error_handler;
	}

	/**
	 * initialize sea_funcs.
	 */
	ctx->f = (struct sea_funcs *)malloc(sizeof(struct sea_funcs));
	if(ctx->f == NULL) {
		error_label = SEA_ERROR_OUT_OF_MEM;
		goto _sea_init_error_handler;
	}

	/**
	 * initialize alignment functions
	 */
	int32_t cost_idx = (ctx->flags & SEA_FLAGS_MASK_COST) >> SEA_FLAGS_POS_COST;
	int32_t dp_idx = (ctx->flags & SEA_FLAGS_MASK_DP) >> SEA_FLAGS_POS_DP;

	if(cost_idx >= 3 || dp_idx >= 3) {
		error_label = SEA_ERROR_INVALID_ARGS;
		goto _sea_init_error_handler;
	}

	ctx->f->twig    = func_table[cost_idx][dp_idx][1];
	ctx->f->branch  = func_table[cost_idx][dp_idx][2];
	ctx->f->trunk   = func_table[cost_idx][dp_idx][3];
	ctx->f->balloon = func_table[cost_idx][dp_idx][4];
	ctx->f->bulge   = func_table[cost_idx][dp_idx][5];
	ctx->f->cap     = func_table[cost_idx][dp_idx][6];

	if(ctx->f->twig == NULL || ctx->f->branch == NULL || ctx->f->trunk == NULL
		|| ctx->f->balloon == NULL || ctx->f->bulge == NULL || ctx->f->cap == NULL) {
		error_label = SEA_ERROR_INVALID_ARGS;
		goto _sea_init_error_handler;
	}

	/**
	 * set seq a reader functions
	 */
	int32_t popa_idx = (ctx->flags & SEA_FLAGS_MASK_SEQ_A) >> SEA_FLAGS_POS_SEQ_A;
	int32_t popb_idx = (ctx->flags & SEA_FLAGS_MASK_SEQ_B) >> SEA_FLAGS_POS_SEQ_B;
	if(popa_idx >= 8 || popb_idx >= 8) {
		error_label = SEA_ERROR_INVALID_ARGS;
		goto _sea_init_error_handler;
	}
	ctx->f->popa = rd_table[popa_idx];
	ctx->f->popb = rd_table[popb_idx];
	if(ctx->f->popa == NULL || ctx->f->popb == NULL) {
		error_label = SEA_ERROR_INVALID_ARGS;
		goto _sea_init_error_handler;
	}

	/**
	 * set alignment writer functions
	 */
	int32_t aln_idx = (ctx->flags & SEA_FLAGS_MASK_ALN) >> SEA_FLAGS_POS_ALN;
	if(aln_idx >= 4) {
		error_label = SEA_ERROR_INVALID_ARGS;
		goto _sea_init_error_handler;
	}
	ctx->f->init_f = wr_table[aln_idx][0][0];
	ctx->f->pushm_f = wr_table[aln_idx][0][1];
	ctx->f->pushx_f = wr_table[aln_idx][0][2];
	ctx->f->pushi_f = wr_table[aln_idx][0][3];
	ctx->f->pushd_f = wr_table[aln_idx][0][4];
	ctx->f->finish_f = wr_table[aln_idx][0][5];

	ctx->f->init_r = wr_table[aln_idx][1][0];
	ctx->f->pushm_r = wr_table[aln_idx][1][1];
	ctx->f->pushx_r = wr_table[aln_idx][1][2];
	ctx->f->pushi_r = wr_table[aln_idx][1][3];
	ctx->f->pushd_r = wr_table[aln_idx][1][4];
	ctx->f->finish_r = wr_table[aln_idx][1][5];

	if(ctx->f->init_f == NULL || ctx->f->pushm_f == NULL || ctx->f->pushx_f == NULL
		|| ctx->f->pushi_f == NULL || ctx->f->pushd_f == NULL || ctx->f->finish_f == NULL
		|| ctx->f->init_r == NULL || ctx->f->pushm_r == NULL || ctx->f->pushx_r == NULL
		|| ctx->f->pushi_r == NULL || ctx->f->pushd_r == NULL || ctx->f->finish_r == NULL) {
		error_label = SEA_ERROR_INVALID_ARGS;
		goto _sea_init_error_handler;
	}

	/**
	 * initialize init vector
	 */
	ctx->iv = (struct sea_ivec *)malloc(sizeof(struct sea_ivec) + 32*sizeof(uint8_t));
	ctx->iv->pv = ctx->iv + 1;
	ctx->iv->cv = ctx->iv->pv + ctx->bw/2;
	ctx->iv->clen = ctx->iv->plen = ctx->bw/2;
	ctx->iv->size = sizeof(int8_t);
	#define _Q(x)		( (x) - ctx->bw/4 )
	for(i = 0; i < ctx->bw/2; i++) {
		debug("pv: i(%d)", i);
		((int8_t *)ctx->iv->pv)[i] = -ctx->gi + (_Q(i) < 0 ? -_Q(i) : _Q(i)+1) * (2 * ctx->gi - ctx->m);
		debug("cv: i(%d)", i);
		((int8_t *)ctx->iv->cv)[i] =            (_Q(i) < 0 ? -_Q(i) : _Q(i)  ) * (2 * ctx->gi - ctx->m);
	}
	#undef _Q

	return(ctx);

_sea_init_error_handler:
	if(ctx != NULL) {
		if(ctx->f != NULL) {
			free(ctx->f);
			ctx->f = NULL;
		}
		memset(ctx, 0, sizeof(struct sea_context));
		ctx->alg = error_label;
	}
	return(ctx);
}

/**
 * @fn sea_align_intl
 *
 * @brief (internal) the body of sea_align function.
 */
static
sea_res_t *sea_align_intl(
	sea_t const *ctx,
	void const *a,
	int64_t asp,
	int64_t aep,
	void const *b,
	int64_t bsp,
	int64_t bep,
	uint8_t const *guide,
	int64_t glen,
	int32_t dir)
{
	int8_t bw = ctx->bw;
	struct sea_process c;
	sea_res_t *r = NULL;
	int32_t error_label = SEA_ERROR;

	debug("enter sea_align");
	/* check if ctx points to valid context */
	if(ctx == NULL) {
		debug("invalid context: ctx(%p)", ctx);
		error_label = SEA_ERROR_INVALID_CONTEXT;
		goto _sea_error_handler;
	}

	/* check if the pointers, start position values, extension length values are proper. */
	if(a == NULL || b == NULL || asp < 0 || aep < asp || bsp < 0 || bep < bsp) {
		debug("invalid args: a(%p), apos(%lld), alen(%lld), b(%p), bpos(%lld), blen(%lld)",
			a, asp, aep, b, bsp, bep);
		error_label = SEA_ERROR_INVALID_ARGS;
		goto _sea_error_handler;
	}

	/**
	 * if one of the length is zero, returns with score = 0 and aln = "".
	 */
	if(asp == aep || bsp == bep) {
		r = (sea_res_t *)malloc(sizeof(struct sea_result) + 1);
		r->a = a;
		r->apos = asp;
		r->alen = aep;
		r->b = b;
		r->bpos = bsp;
		r->blen = bep;
		r->len = 0;
		r->score = 0;
		r->aln = (void *)(r + 1);
		*((uint8_t *)r->aln) = '\0';
		r->ctx = ctx;
		return(r);
	}

	/**
	 * initialize local context
	 */

	/** initialize init vector */
	debug("initialize vector");
	c.v = *(ctx->iv);

	/** initialize coordinates */
	debug("initialize coordinates");
	c.i = asp + bw/4;								/** the top-right cell of the lane */
	c.j = bsp - bw/4;
	c.p = 0; //COP(asp, bsp, ctx->bw);
	c.q = 0;
	c.mi = asp;
	c.mj = bsp;
	c.mp = 0; //COP(asp, bsp, ctx->bw);
	c.mq = 0;

	/** initialize sequence reader */
	rd_init(c.a, ctx->f->popa, a);
	rd_init(c.b, ctx->f->popb, b);
	c.asp = asp;
	c.bsp = bsp;
	c.aep = aep;
	c.bep = bep;
	c.alim = c.aep - bw/2;
	c.blim = c.bep - bw/2;

	/** initialize alignment writer */
	wr_init(c.l, ctx->f, dir);

	/** initialize memory */
	c.size = ctx->isize;
	AMALLOC(c.dp.sp, c.size, ctx->memaln);
	if(c.dp.sp == NULL) {
		error_label = SEA_ERROR_OUT_OF_MEM;
		goto _sea_error_handler;
	}
	c.dp.ep = (c.pdp = c.dp.sp) + c.size;

	c.dr.sp = malloc(c.size);
	if(c.dr.sp == NULL) {
		error_label = SEA_ERROR_OUT_OF_MEM;
		goto _sea_error_handler;
	}
	c.dr.ep = (c.pdr = c.dr.sp) + c.size;
	c.pdr[0] = LEFT;		/** initial vector */

	/**
	 * initialize max
	 */
	c.max = 0;

	/* do alignment */
	error_label = ctx->f->twig(ctx, &c);
	if(error_label != SEA_SUCCESS) {
		goto _sea_error_handler;					/** when the path went out of the band */
	}

	/* finishing */
	wr_finish(c.l, ctx, dir);
	r = (sea_res_t *)c.l.p;

	r->aln = (uint8_t *)c.l.p + c.l.pos;
	r->len = c.l.size;
	debug("finishing: len(%lld)", r->len);
	r->a = a;
	r->b = b;
	r->apos = c.mi;
	r->bpos = c.mj;
	r->alen = c.aep;
	r->blen = c.bep;
	r->score = c.max;
	r->ctx = ctx;

	/* clean DP matrix */
//	AFREE(iv, 2*bw);
	AFREE(c.dp.sp, ctx->isize);
//	AFREE(c.dr.sp, ctx->isize);
	free(c.dr.sp);

	return(r);

_sea_error_handler:
	r = (sea_res_t *)malloc(sizeof(struct sea_result) + 1);
	r->a = NULL;
	r->apos = 0;
	r->alen = 0;
	r->b = NULL;
	r->bpos = 0;
	r->blen = 0;
	r->len = 0;
	r->score = error_label;
	r->aln = (void *)(r + 1);
	*((uint8_t *)r->aln) = '\0';
	r->ctx = ctx;
	return(r);
}

/**
 * @fn sea_align
 *
 * @brief alignment function. 
 *
 * @param[ref] ctx : a pointer to an alignment context structure. must be initialized with sea_init function.
 * @param[in] a : a pointer to the query sequence a. see seqreader.h for more details of query sequence formatting.
 * @param[in] apos : the alignment start position on a. (0 <= apos < length(sequence a)) (or search start position in the Smith-Waterman algorithm). the alignment includes the position apos.
 * @param[in] alen : the extension length on a. (0 < alen) (to be exact, alen is search area length in the Smith-Waterman algorithm. the maximum extension length in the seed-and-extend alignment algorithm. the length of the query a to be aligned in the Needleman-Wunsch algorithm.)
 * @param[in] b : a pointer to the query sequence b.
 * @param[in] bpos : the alignment start position on b. (0 <= bpos < length(sequence b))
 * @param[in] blen : the extension length on b. (0 < blen)
 *
 * @return an pointer to the sea_result structure.
 *
 * @sa sea_init
 */
sea_res_t *sea_align(
	sea_t const *ctx,
	void const *a,
	int64_t asp,
	int64_t aep,
	void const *b,
	int64_t bsp,
	int64_t bep,
	uint8_t const *guide,
	int64_t glen)
{
	return(sea_align_intl(ctx, a, asp, aep, b, bsp, bep, guide, glen, ALN_FW));
}

/**
 * @fn sea_align_f
 * @brief the same as sea_align.
 */
sea_res_t *sea_align_f(
	sea_t const *ctx,
	void const *a,
	int64_t asp,
	int64_t aep,
	void const *b,
	int64_t bsp,
	int64_t bep,
	uint8_t const *guide,
	int64_t glen)
{
	return(sea_align_intl(ctx, a, asp, aep, b, bsp, bep, guide, glen, ALN_FW));
}

/**
 * @fn sea_align_r
 * @brief the reverse variant of sea_align.
 */
sea_res_t *sea_align_r(
	sea_t const *ctx,
	void const *a,
	int64_t asp,
	int64_t aep,
	void const *b,
	int64_t bsp,
	int64_t bep,
	uint8_t const *guide,
	int64_t glen)
{
	return(sea_align_intl(ctx, a, asp, aep, b, bsp, bep, guide, glen, ALN_RV));
}

/**
 * @fn sea_get_error_num
 *
 * @brief extract error number from a context or a result
 *
 * @param[ref] ctx : a pointer to an alignment context structure. can be NULL.
 * @param[ref] aln : a pointer to a result structure. can be NULL.
 *
 * @return error number, defined in sea_error
 */
int sea_get_error_num(
	sea_t const *ctx,
	sea_res_t *aln)
{
	int32_t error_label = SEA_SUCCESS;
	if(aln != NULL) {
		error_label = aln->score;
	}
	return error_label;
}

/**
 * @fn sea_aln_free
 *
 * @brief clean up sea_result structure
 *
 * @param[in] aln : an pointer to sea_result structure.
 *
 * @return none.
 *
 * @sa sea_sea
 */
void sea_aln_free(
	sea_t const *ctx,
	sea_res_t *aln)
{
	if(aln != NULL) {
		free(aln);
		return;
	}
	return;
}

/**
 * @fn sea_clean
 *
 * @brief clean up the alignment context structure.
 *
 * @param[in] ctx : a pointer to the alignment structure.
 *
 * @return none.
 *
 * @sa sea_init
 */
void sea_clean(
	sea_t *ctx)
{
	if(ctx != NULL) {
		if(ctx->f != NULL) {
			free(ctx->f); ctx->f = NULL;
		}
		if(ctx->iv != NULL) {
			free(ctx->iv); ctx->iv = NULL;
		}
		free(ctx);
		return;
	}
	return;
}

/*
 * end of sea.c
 */
