#include "param.h"

/* Enable extra kernel-side diagnostics: extraction debug and per-round counters */
#define DEBUG_EXTRACTION


#pragma OPENCL EXTENSION cl_khr_global_int32_base_atomics : enable

#ifdef DEBUG_EXTRACTION
typedef struct extraction_debug_s {
	uint round;
	uint thread_id;
	uint row;
	uint slot;
	ulong xi0;
	ulong xi1;
	ulong xi2;
	ulong xi3;
	/* first 32 bytes of the stored slot (copied for diagnostics) */
	ulong stored0;
	ulong stored1;
	ulong stored2;
	ulong stored3;
	uint status; /* 1=xor_nonzero, 2=stored, 3=overflow */
	uint table_half; /* which buf_ht half was written (round % 2) */
	ulong xi_sig; /* simple signature: dbg_xi0 ^ dbg_xi1 ^ dbg_xi2 ^ dbg_xi3 */
} extraction_debug_t;
#define EXTRACTION_DEBUG_ENTRIES 4096
#endif

#ifdef DEBUG_EXTRACTION
/* Per-insert snapshots (store first 32 bytes of the slot for each insert) */
#define SNAPSHOT_ENTRIES 65536
#endif

#ifdef DEBUG_EXTRACTION
/* Add an extra device-side counter for per-snapshot sequencing */
#define HT_DBG_ARGS , __global extraction_debug_t *extraction_dbg, __global uint *extraction_dbg_counter, __global ulong *snapshot_buf, __global uint *snapshot_counter, __global uint *snapshot_seq_counter
#define HT_DBG_PASS , extraction_dbg, extraction_dbg_counter, snapshot_buf, snapshot_counter, snapshot_seq_counter
#else
#define HT_DBG_ARGS
#define HT_DBG_PASS
#endif

/* Per-round counters (always enabled for diagnostics) */
#define PER_ROUND_COUNTS
#define ROUND_CNT_ARGS , __global uint *round_collisions, __global uint *round_stored
#define ROUND_CNT_PASS , round_collisions, round_stored


/*
** Equihash 192,7 layout (length in bytes):
** round 0: cnt(4) i(4) Xi(22) pad(2)
** round 1: cnt(4) i(4) i(4) Xi(19) pad(5)
** round 2: cnt(4) i(4) i(4) i(4) Xi(16) pad(8)
** round 3: cnt(4) i(4) i(4) i(4) i(4) Xi(13) pad(11)
** round 4: cnt(4) i(4) i(4) i(4) i(4) i(4) Xi(10) pad(14)
** round 5: cnt(4) i(4) i(4) i(4) i(4) i(4) i(4) Xi(7) pad(17)
** round 6: cnt(4) i(4) i(4) i(4) i(4) i(4) i(4) i(4) Xi(4) pad(20)
**
** 24-bit reduction per round, 7 rounds, 400-byte solution.
*/

__constant ulong blake_iv[] =
{
    0x6a09e667f3bcc908, 0xbb67ae8584caa73b,
    0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
    0x510e527fade682d1, 0x9b05688c2b3e6c1f,
    0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
};

/*
** Reset counters in hash table.
*/
__kernel
void kernel_init_ht(__global char *ht, __global uint *rowCounters)
{
    rowCounters[get_global_id(0)] = 0;
}

/*
** If xi0,xi1,xi2,xi3 are stored consecutively in little endian then they
** represent (hex notation, group of 5 hex digits are a group of PREFIX bits):
**   aa aa ab bb bb cc cc cd dd...  [round 0]
**         --------------------
**      ...ab bb bb cc cc cd dd...  [odd round]
**               --------------
**               ...cc cc cd dd...  [next even round]
**                        -----
** Bytes underlined are going to be stored in the slot. Preceding bytes
** (and possibly part of the underlined bytes, depending on NR_ROWS_LOG) are
** used to compute the row number.
**
** Round 0: xi0,xi1,xi2,xi3 is a 25-byte Xi (xi3: only the low byte matter)
** Round 1: xi0,xi1,xi2 is a 23-byte Xi (incl. the colliding PREFIX nibble)
** TODO: update lines below with padding nibbles
** Round 2: xi0,xi1,xi2 is a 20-byte Xi (xi2: only the low 4 bytes matter)
** Round 3: xi0,xi1,xi2 is a 17.5-byte Xi (xi2: only the low 1.5 bytes matter)
** Round 4: xi0,xi1 is a 15-byte Xi (xi1: only the low 7 bytes matter)
** Round 5: xi0,xi1 is a 12.5-byte Xi (xi1: only the low 4.5 bytes matter)
** Round 6: xi0,xi1 is a 10-byte Xi (xi1: only the low 2 bytes matter)
** Round 7: xi0 is a 7.5-byte Xi (xi0: only the low 7.5 bytes matter)
** Round 8: xi0 is a 5-byte Xi (xi0: only the low 5 bytes matter)
**
** Return 0 if successfully stored, or 1 if the row overflowed.
*/
/* forward declaration so ht_store can call half_aligned_long */
ulong half_aligned_long(__global ulong *p, uint offset);

uint ht_store(uint round, __global char *ht, uint i,
	ulong xi0, ulong xi1, ulong xi2, ulong xi3, __global uint *rowCounters ROUND_CNT_ARGS HT_DBG_ARGS)
{
    uint    row;
    __global char       *p;
    uint                cnt;
#if NR_ROWS_LOG == 16
    if (!(round % 2))
	row = (xi0 & 0xffff);
    else
	// if we have in hex: "ab cd ef..." (little endian xi0) then this
	// formula computes the row as 0xdebc. it skips the 'a' nibble as it
	// is part of the PREFIX. The Xi will be stored starting with "ef...";
	// 'e' will be considered padding and 'f' is part of the current PREFIX
	row = ((xi0 & 0xf00) << 4) | ((xi0 & 0xf00000) >> 12) |
	    ((xi0 & 0xf) << 4) | ((xi0 & 0xf000) >> 12);
#elif NR_ROWS_LOG == 18
    if (!(round % 2))
	row = (xi0 & 0xffff) | ((xi0 & 0xc00000) >> 6);
    else
	row = ((xi0 & 0xc0000) >> 2) |
	    ((xi0 & 0xf00) << 4) | ((xi0 & 0xf00000) >> 12) |
	    ((xi0 & 0xf) << 4) | ((xi0 & 0xf000) >> 12);
#elif NR_ROWS_LOG == 19
    if (!(round % 2))
	row = (xi0 & 0xffff) | ((xi0 & 0xe00000) >> 5);
    else
	row = ((xi0 & 0xe0000) >> 1) |
	    ((xi0 & 0xf00) << 4) | ((xi0 & 0xf00000) >> 12) |
	    ((xi0 & 0xf) << 4) | ((xi0 & 0xf000) >> 12);
#elif NR_ROWS_LOG == 20
    if (!(round % 2))
	row = (xi0 & 0xffff) | ((xi0 & 0xf00000) >> 4);
    else
	row = ((xi0 & 0xf0000) >> 0) |
	    ((xi0 & 0xf00) << 4) | ((xi0 & 0xf00000) >> 12) |
	    ((xi0 & 0xf) << 4) | ((xi0 & 0xf000) >> 12);
#else

#endif
	/* Keep originals for extraction debugging before the 16-bit rotation */
#ifdef DEBUG_EXTRACTION
	ulong dbg_xi0 = xi0;
	ulong dbg_xi1 = xi1;
	ulong dbg_xi2 = xi2;
	ulong dbg_xi3 = xi3;
#endif
	xi0 = (xi0 >> 24) | (xi1 << (64 - 24));
	xi1 = (xi1 >> 24) | (xi2 << (64 - 24));
	xi2 = (xi2 >> 24) | (xi3 << (64 - 24));
    p = ht + row * NR_SLOTS * SLOT_LEN;
    uint rowIdx = row/ROWS_PER_UINT;
    uint rowOffset = BITS_PER_ROW*(row%ROWS_PER_UINT);
    uint xcnt = atomic_add(rowCounters + rowIdx, 1 << rowOffset);
    xcnt = (xcnt >> rowOffset) & ROW_MASK;
    cnt = xcnt;
	    if (cnt >= NR_SLOTS)
      {
	// avoid overflows
	atomic_sub(rowCounters + rowIdx, 1 << rowOffset);
			/* Log overflow attempt (sample round 0 to avoid saturation) */
			#ifdef DEBUG_EXTRACTION
				{
					uint do_log = 1;
					if (do_log) {
					uint idx = atomic_inc(extraction_dbg_counter);
					if (idx < EXTRACTION_DEBUG_ENTRIES)
					{
						extraction_dbg[idx].round = round;
						extraction_dbg[idx].thread_id = get_global_id(0);
						extraction_dbg[idx].row = row;
						extraction_dbg[idx].slot = cnt;
						extraction_dbg[idx].xi0 = dbg_xi0;
						extraction_dbg[idx].xi1 = dbg_xi1;
						extraction_dbg[idx].xi2 = dbg_xi2;
						extraction_dbg[idx].xi3 = dbg_xi3;
						extraction_dbg[idx].status = 3; /* overflow */
						/* copy first 32 bytes as actually written into HT */
						extraction_dbg[idx].stored0 = half_aligned_long((__global ulong *)p, 0);
						extraction_dbg[idx].stored1 = half_aligned_long((__global ulong *)p, 8);
						extraction_dbg[idx].stored2 = half_aligned_long((__global ulong *)p, 16);
						extraction_dbg[idx].stored3 = half_aligned_long((__global ulong *)p, 24);
						extraction_dbg[idx].table_half = round & 1;
						extraction_dbg[idx].xi_sig = dbg_xi0 ^ dbg_xi1 ^ dbg_xi2 ^ dbg_xi3;
							/* For diagnostics: also write per-insert snapshot entries */
							if (snapshot_buf) {
								/* write at extraction index so it's 1:1 with extraction_dbg entries */
								snapshot_buf[idx * 8 + 0] = extraction_dbg[idx].stored0;
								snapshot_buf[idx * 8 + 1] = extraction_dbg[idx].stored1;
								snapshot_buf[idx * 8 + 2] = extraction_dbg[idx].stored2;
								snapshot_buf[idx * 8 + 3] = extraction_dbg[idx].stored3;
								snapshot_buf[idx * 8 + 4] = (ulong)extraction_dbg[idx].thread_id;
								snapshot_buf[idx * 8 + 5] = (ulong)extraction_dbg[idx].table_half;
								/* marker: combine thread and extraction index */
								snapshot_buf[idx * 8 + 6] = ((ulong)extraction_dbg[idx].thread_id << 32) | (ulong)idx;
								if (snapshot_seq_counter) {
									uint seqv = atomic_inc(snapshot_seq_counter);
									snapshot_buf[idx * 8 + 7] = (ulong)seqv;
								} else {
									snapshot_buf[idx * 8 + 7] = 0;
								}
							}
							/* For diagnostics: write snapshot indexed by extraction dbg index
							 * This avoids relying on snapshot_counter atomic increments which
							 * may not be available/working on some drivers. We store at idx
							 * so snapshots correspond 1:1 with extraction_dbg entries (up to
							 * EXTRACTION_DEBUG_ENTRIES). */
							if (snapshot_buf) {
							 /* store 4x64-bit stored words then metadata: thread_id, table_half, seq */
							 snapshot_buf[idx * 8 + 0] = extraction_dbg[idx].stored0;
							 snapshot_buf[idx * 8 + 1] = extraction_dbg[idx].stored1;
							 snapshot_buf[idx * 8 + 2] = extraction_dbg[idx].stored2;
							 snapshot_buf[idx * 8 + 3] = extraction_dbg[idx].stored3;
							 snapshot_buf[idx * 8 + 4] = (ulong)extraction_dbg[idx].thread_id;
							 snapshot_buf[idx * 8 + 5] = (ulong)extraction_dbg[idx].table_half;
							 /* per-snapshot seq (increment device-side counter) */
							 /* Use the extraction debug index as a stable per-snapshot seq */
								 /* deterministic per-snapshot marker: thread_id<<32 | idx */
								 snapshot_buf[idx * 8 + 6] = ((ulong)extraction_dbg[idx].thread_id << 32) | (ulong)idx;
								/* record a monotonic device-side sequence for this snapshot */
								if (snapshot_seq_counter) {
									uint seqv = atomic_inc(snapshot_seq_counter);
									snapshot_buf[idx * 8 + 7] = (ulong)seqv;
								} else {
									snapshot_buf[idx * 8 + 7] = 0; /* reserved */
								}
							}
								/* snapshot the stored words */
								if (snapshot_counter) {
									uint sidx = atomic_inc(snapshot_counter);
									if (sidx < SNAPSHOT_ENTRIES) {
										snapshot_buf[sidx * 8 + 0] = extraction_dbg[idx].stored0;
										snapshot_buf[sidx * 8 + 1] = extraction_dbg[idx].stored1;
										snapshot_buf[sidx * 8 + 2] = extraction_dbg[idx].stored2;
										snapshot_buf[sidx * 8 + 3] = extraction_dbg[idx].stored3;
										snapshot_buf[sidx * 8 + 4] = (ulong)extraction_dbg[idx].thread_id;
										snapshot_buf[sidx * 8 + 5] = (ulong)extraction_dbg[idx].table_half;
										/* deterministic per-snapshot marker: thread_id<<32 | sidx */
										snapshot_buf[sidx * 8 + 6] = ((ulong)extraction_dbg[idx].thread_id << 32) | (ulong)sidx;
										if (snapshot_seq_counter) {
											uint seqv = atomic_inc(snapshot_seq_counter);
											snapshot_buf[sidx * 8 + 7] = (ulong)seqv;
										} else {
											snapshot_buf[sidx * 8 + 7] = 0;
										}
									}
								}
						/* also store a short snapshot (4x64-bit words) for host-side per-insert inspection */
						if (snapshot_counter) {
							uint sidx = atomic_inc(snapshot_counter);
							if (sidx < SNAPSHOT_ENTRIES) {
								snapshot_buf[sidx * 8 + 0] = extraction_dbg[idx].stored0;
								snapshot_buf[sidx * 8 + 1] = extraction_dbg[idx].stored1;
								snapshot_buf[sidx * 8 + 2] = extraction_dbg[idx].stored2;
								snapshot_buf[sidx * 8 + 3] = extraction_dbg[idx].stored3;
								snapshot_buf[sidx * 8 + 4] = (ulong)extraction_dbg[idx].thread_id;
								snapshot_buf[sidx * 8 + 5] = (ulong)extraction_dbg[idx].table_half;
								snapshot_buf[sidx * 8 + 6] = ((ulong)extraction_dbg[idx].thread_id << 32) | (ulong)sidx;
								snapshot_buf[sidx * 8 + 7] = 0;
							}
						}
					}
				}
			}
			#endif
			return 1;
      }
    p += cnt * SLOT_LEN + xi_offset_for_round(round);
    // store "i" (always 4 bytes before Xi)
    *(__global uint *)(p - 4) = i;
    if (round == 0 || round == 1)
      {
	// store 24 bytes
	*(__global ulong *)(p + 0) = xi0;
	*(__global ulong *)(p + 8) = xi1;
	*(__global ulong *)(p + 16) = xi2;
      }
    else if (round == 2)
      {
	// store 20 bytes
	*(__global uint *)(p + 0) = xi0;
	*(__global ulong *)(p + 4) = (xi0 >> 32) | (xi1 << 32);
	*(__global ulong *)(p + 12) = (xi1 >> 32) | (xi2 << 32);
      }
    else if (round == 3)
      {
	// store 16 bytes
	*(__global uint *)(p + 0) = xi0;
	*(__global ulong *)(p + 4) = (xi0 >> 32) | (xi1 << 32);
	*(__global uint *)(p + 12) = (xi1 >> 32);
      }
    else if (round == 4)
      {
	// store 16 bytes
	*(__global ulong *)(p + 0) = xi0;
	*(__global ulong *)(p + 8) = xi1;
      }
    else if (round == 5)
      {
	// store 12 bytes
	*(__global ulong *)(p + 0) = xi0;
	*(__global uint *)(p + 8) = xi1;
      }
		else if (round == 6 || round == 7)
			{
		// store 8 bytes
		*(__global uint *)(p + 0) = xi0;
		*(__global uint *)(p + 4) = (xi0 >> 32);
			}
	#ifdef DEBUG_EXTRACTION
				{
				uint do_log = 1;
			if (do_log) {
				uint idx = atomic_inc(extraction_dbg_counter);
				if (idx < EXTRACTION_DEBUG_ENTRIES)
				{
					extraction_dbg[idx].round = round;
					extraction_dbg[idx].thread_id = get_global_id(0);
					extraction_dbg[idx].row = row;
					extraction_dbg[idx].slot = cnt;
					extraction_dbg[idx].xi0 = dbg_xi0;
					extraction_dbg[idx].xi1 = dbg_xi1;
					extraction_dbg[idx].xi2 = dbg_xi2;
					extraction_dbg[idx].xi3 = dbg_xi3;
					extraction_dbg[idx].status = 2; /* stored */
					/* copy first 32 bytes of stored slot for offline inspection */
					extraction_dbg[idx].stored0 = half_aligned_long((__global ulong *)p, 0);
					extraction_dbg[idx].stored1 = half_aligned_long((__global ulong *)p, 8);
					extraction_dbg[idx].stored2 = half_aligned_long((__global ulong *)p, 16);
					extraction_dbg[idx].stored3 = half_aligned_long((__global ulong *)p, 24);
					extraction_dbg[idx].table_half = round & 1;
					extraction_dbg[idx].xi_sig = dbg_xi0 ^ dbg_xi1 ^ dbg_xi2 ^ dbg_xi3;
					/* For diagnostics: write per-insert snapshot entries for stored events */
					if (snapshot_buf) {
						/* write at extraction index so it's 1:1 with extraction_dbg entries */
						snapshot_buf[idx * 8 + 0] = extraction_dbg[idx].stored0;
						snapshot_buf[idx * 8 + 1] = extraction_dbg[idx].stored1;
						snapshot_buf[idx * 8 + 2] = extraction_dbg[idx].stored2;
						snapshot_buf[idx * 8 + 3] = extraction_dbg[idx].stored3;
						snapshot_buf[idx * 8 + 4] = (ulong)extraction_dbg[idx].thread_id;
						snapshot_buf[idx * 8 + 5] = (ulong)extraction_dbg[idx].table_half;
						/* marker: combine thread and extraction index */
						snapshot_buf[idx * 8 + 6] = ((ulong)extraction_dbg[idx].thread_id << 32) | (ulong)idx;
						if (snapshot_seq_counter) {
							uint seqv = atomic_inc(snapshot_seq_counter);
							snapshot_buf[idx * 8 + 7] = (ulong)seqv;
						} else {
							snapshot_buf[idx * 8 + 7] = 0; /* reserved */
						}
					}
				}
			}
		}
	#endif
		/* account a successful store for this round */
		#ifdef PER_ROUND_COUNTS
		atomic_inc(round_stored + round);
		#endif
		return 0;
}

#define mix(va, vb, vc, vd, x, y) \
    va = (va + vb + x); \
vd = rotate((vd ^ va), (ulong)64 - 32); \
vc = (vc + vd); \
vb = rotate((vb ^ vc), (ulong)64 - 24); \
va = (va + vb + y); \
vd = rotate((vd ^ va), (ulong)64 - 16); \
vc = (vc + vd); \
vb = rotate((vb ^ vc), (ulong)64 - 63);

/*
** Execute round 0 (blake).
**
** Note: making the work group size less than or equal to the wavefront size
** allows the OpenCL compiler to remove the barrier() calls, see "2.2 Local
** Memory (LDS) Optimization 2-10" in:
** http://developer.amd.com/tools-and-sdks/opencl-zone/amd-accelerated-parallel-processing-app-sdk/opencl-optimization-guide/
*/
__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void kernel_round0(__global ulong *blake_state, __global char *ht,
	__global uint *rowCounters, __global uint *debug ROUND_CNT_ARGS HT_DBG_ARGS)
{
    uint                tid = get_global_id(0);
	/* Deterministic self-test: thread 0 writes a known 32-byte pattern into
	 * a chosen slot at the Xi offset and reads it back into extraction_dbg[0]
	 * so the host can verify offsets/endianness directly. Enabled only when
	 * DEBUG_EXTRACTION is defined. */
#ifdef DEBUG_EXTRACTION
	if (tid == 0) {
		uint test_row = 0; /* choose row 0 for deterministic test */
		uint test_slot = 0; /* choose slot 0 */
		uint test_round = 0; /* use round 0 xi_offset */
		__global char *base = ht + test_row * NR_SLOTS * SLOT_LEN + test_slot * SLOT_LEN + xi_offset_for_round(test_round);
		/* write four distinct 8-byte words so they're easy to spot */
		*(__global ulong *)(base + 0) = (ulong)0x1122334455667788ULL;
		*(__global ulong *)(base + 8) = (ulong)0x99aabbccddeeff00ULL;
		*(__global ulong *)(base + 16) = (ulong)0x0102030405060708ULL;
		*(__global ulong *)(base + 24) = (ulong)0xdeadbeefcafebabeULL;
		/* read back via half_aligned_long and record into extraction_dbg[0] */
		uint idx = atomic_inc(extraction_dbg_counter);
		if (idx < EXTRACTION_DEBUG_ENTRIES) {
			extraction_dbg[idx].round = test_round;
			extraction_dbg[idx].thread_id = tid;
			extraction_dbg[idx].row = test_row;
			extraction_dbg[idx].slot = test_slot;
			extraction_dbg[idx].xi0 = 0; extraction_dbg[idx].xi1 = 0; extraction_dbg[idx].xi2 = 0; extraction_dbg[idx].xi3 = 0;
			extraction_dbg[idx].status = 0xdeadbeef;
			extraction_dbg[idx].stored0 = half_aligned_long((__global ulong *)base, 0);
			extraction_dbg[idx].stored1 = half_aligned_long((__global ulong *)base, 8);
			extraction_dbg[idx].stored2 = half_aligned_long((__global ulong *)base, 16);
			extraction_dbg[idx].stored3 = half_aligned_long((__global ulong *)base, 24);
			extraction_dbg[idx].table_half = test_round & 1;
			extraction_dbg[idx].xi_sig = 0x123456789abcdef0ULL;
				/* also snapshot deterministic test pattern */
				if (snapshot_counter) {
					uint sidx = atomic_inc(snapshot_counter);
					if (sidx < SNAPSHOT_ENTRIES) {
						snapshot_buf[sidx * 8 + 0] = extraction_dbg[idx].stored0;
						snapshot_buf[sidx * 8 + 1] = extraction_dbg[idx].stored1;
						snapshot_buf[sidx * 8 + 2] = extraction_dbg[idx].stored2;
						snapshot_buf[sidx * 8 + 3] = extraction_dbg[idx].stored3;
						snapshot_buf[sidx * 8 + 4] = (ulong)extraction_dbg[idx].thread_id;
						snapshot_buf[sidx * 8 + 5] = (ulong)extraction_dbg[idx].table_half;
						snapshot_buf[sidx * 8 + 6] = ((ulong)extraction_dbg[idx].thread_id << 32) | (ulong)sidx;
						snapshot_buf[sidx * 8 + 7] = 0;
					}
				}
		}
	}
#endif
/*
 * Optional debug: force every work-item to emit a snapshot entry so we can
 * validate snapshot counter/seq atomics and host readback independently of
 * the store path. Enable by defining DEBUG_FORCE_SNAPSHOT when building.
 */
#ifdef DEBUG_FORCE_SNAPSHOT
	{
		uint __g = get_global_id(0);
		/* allocate one snapshot slot per emitter via atomic_inc */
		if (snapshot_counter) {
			uint __sidx = atomic_inc(snapshot_counter);
			if (__sidx < SNAPSHOT_ENTRIES) {
				ulong base_idx = (ulong)__sidx * 8UL;
				snapshot_buf[base_idx + 0] = (ulong)__g; /* test payload */
				snapshot_buf[base_idx + 1] = (ulong)0xfeedfacecafebabeULL;
				snapshot_buf[base_idx + 2] = (ulong)0x0123456789abcdefULL;
				snapshot_buf[base_idx + 3] = 0UL;
				snapshot_buf[base_idx + 4] = (ulong)__g; /* thread id */
				snapshot_buf[base_idx + 5] = 0UL; /* table half */
				snapshot_buf[base_idx + 6] = ((ulong)__g << 32) | (ulong)__sidx; /* marker */
				if (snapshot_seq_counter) {
					uint __seq = atomic_inc(snapshot_seq_counter);
					snapshot_buf[base_idx + 7] = (ulong)__seq;
				} else {
					snapshot_buf[base_idx + 7] = 0UL;
				}
			}
		}
	}
#endif

	ulong               v[16];
    uint                inputs_per_thread = NR_INPUTS / get_global_size(0);
    uint                input = tid * inputs_per_thread;
    uint                input_end = (tid + 1) * inputs_per_thread;
    uint                dropped = 0;
    while (input < input_end)
      {
	// shift "i" to occupy the high 32 bits of the second ulong word in the
	// message block
	ulong word1 = (ulong)input << 32;
	// init vector v
	v[0] = blake_state[0];
	v[1] = blake_state[1];
	v[2] = blake_state[2];
	v[3] = blake_state[3];
	v[4] = blake_state[4];
	v[5] = blake_state[5];
	v[6] = blake_state[6];
	v[7] = blake_state[7];
	v[8] =  blake_iv[0];
	v[9] =  blake_iv[1];
	v[10] = blake_iv[2];
	v[11] = blake_iv[3];
	v[12] = blake_iv[4];
	v[13] = blake_iv[5];
	v[14] = blake_iv[6];
	v[15] = blake_iv[7];
	// mix in length of data
	v[12] ^= ZCASH_BLOCK_HEADER_LEN + 4 /* length of "i" */;
	// last block
	v[14] ^= (ulong)-1;

	// round 1
	mix(v[0], v[4], v[8],  v[12], 0, word1);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 2
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], word1, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 3
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, word1);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 4
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, word1);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 5
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, word1);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 6
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], word1, 0);
	// round 7
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], word1, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 8
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, word1);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 9
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], word1, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 10
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], word1, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 11
	mix(v[0], v[4], v[8],  v[12], 0, word1);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 12
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], word1, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);

	// compress v into the blake state; this produces the 50-byte hash
	// (two Xi values)
	ulong h[7];
	h[0] = blake_state[0] ^ v[0] ^ v[8];
	h[1] = blake_state[1] ^ v[1] ^ v[9];
	h[2] = blake_state[2] ^ v[2] ^ v[10];
	h[3] = blake_state[3] ^ v[3] ^ v[11];
	h[4] = blake_state[4] ^ v[4] ^ v[12];
	h[5] = blake_state[5] ^ v[5] ^ v[13];
	h[6] = (blake_state[6] ^ v[6] ^ v[14]) & 0xffff;

	// store the two Xi values in the hash table
#if ZCASH_HASH_LEN == 48
	dropped += ht_store(0, ht, input * 2,
		h[0],
		h[1],
		h[2],
		h[3], rowCounters ROUND_CNT_PASS HT_DBG_PASS);
	/* For Equihash 192,7 the two Xi values align on 8-byte boundaries
	 * and must be passed raw (no 8-bit rotations). The previous shifts
	 * were intended for other parameterizations and corrupt the stored
	 * bytes for n=192,k=7. */
	dropped += ht_store(0, ht, input * 2 + 1,
		h[3],
		h[4],
		h[5],
		h[6], rowCounters ROUND_CNT_PASS HT_DBG_PASS);
#else
#error "unsupported ZCASH_HASH_LEN"
#endif

	input++;
      }
#ifdef DEBUG_EXTRACTION
	/* Persist deterministic test pattern into HT after main loop so host dump
	 * taken later can observe the exact bytes. Thread 0 writes the pattern
	 * into row 0 slot 0 at the xi offset for round 0. */
	if (tid == 0) {
		uint test_row = 0;
		uint test_slot = 0;
		uint test_round = 0;
		__global char *base = ht + test_row * NR_SLOTS * SLOT_LEN + test_slot * SLOT_LEN + xi_offset_for_round(test_round);
		*(__global ulong *)(base + 0) = (ulong)0x1122334455667788ULL;
		*(__global ulong *)(base + 8) = (ulong)0x99aabbccddeeff00ULL;
		*(__global ulong *)(base + 16) = (ulong)0x0102030405060708ULL;
		*(__global ulong *)(base + 24) = (ulong)0xdeadbeefcafebabeULL;
		/* optional: also log an extraction_dbg entry indicating persistent write */
		uint idx2 = atomic_inc(extraction_dbg_counter);
		if (idx2 < EXTRACTION_DEBUG_ENTRIES) {
			extraction_dbg[idx2].round = test_round;
			extraction_dbg[idx2].thread_id = tid;
			extraction_dbg[idx2].row = test_row;
			extraction_dbg[idx2].slot = test_slot;
			extraction_dbg[idx2].status = 0xfeedface;
			extraction_dbg[idx2].stored0 = half_aligned_long((__global ulong *)base, 0);
			extraction_dbg[idx2].stored1 = half_aligned_long((__global ulong *)base, 8);
			extraction_dbg[idx2].stored2 = half_aligned_long((__global ulong *)base, 16);
			extraction_dbg[idx2].stored3 = half_aligned_long((__global ulong *)base, 24);
			extraction_dbg[idx2].table_half = test_round & 1;
			extraction_dbg[idx2].xi_sig = 0xf00dbabecafef00dULL;
		}
	}
#endif
#ifdef ENABLE_DEBUG
    debug[tid * 2] = 0;
    debug[tid * 2 + 1] = dropped;
#endif
}

#if NR_ROWS_LOG <= 16

#define ENCODE_INPUTS(row, slot0, slot1) \
    ((row << 16) | ((slot1 & 0xff) << 8) | (slot0 & 0xff))
#define DECODE_ROW(REF)   (REF >> 16)
#define DECODE_SLOT1(REF) ((REF >> 8) & 0xff)
#define DECODE_SLOT0(REF) (REF & 0xff)

#elif NR_ROWS_LOG == 18

#define ENCODE_INPUTS(row, slot0, slot1) \
    ((row << 14) | ((slot1 & 0x7f) << 7) | (slot0 & 0x7f))
#define DECODE_ROW(REF)   (REF >> 14)
#define DECODE_SLOT1(REF) ((REF >> 7) & 0x7f)
#define DECODE_SLOT0(REF) (REF & 0x7f)

#elif NR_ROWS_LOG == 19

#define ENCODE_INPUTS(row, slot0, slot1) \
    ((row << 13) | ((slot1 & 0x3f) << 6) | (slot0 & 0x3f)) /* 1 spare bit */
#define DECODE_ROW(REF)   (REF >> 13)
#define DECODE_SLOT1(REF) ((REF >> 6) & 0x3f)
#define DECODE_SLOT0(REF) (REF & 0x3f)

#elif NR_ROWS_LOG == 20

#define ENCODE_INPUTS(row, slot0, slot1) \
    ((row << 12) | ((slot1 & 0x3f) << 6) | (slot0 & 0x3f))
#define DECODE_ROW(REF)   (REF >> 12)
#define DECODE_SLOT1(REF) ((REF >> 6) & 0x3f)
#define DECODE_SLOT0(REF) (REF & 0x3f)

#else

#endif

/*
** Access a half-aligned long, that is a long aligned on a 4-byte boundary.
*/
ulong half_aligned_long(__global ulong *p, uint offset)
{
    return
	(((ulong)*(__global uint *)((__global char *)p + offset + 0)) << 0) |
	(((ulong)*(__global uint *)((__global char *)p + offset + 4)) << 32);
}

/* forward declaration so ht_store (which appears earlier) can call it */
ulong half_aligned_long(__global ulong *p, uint offset);

/*
** Access a well-aligned int.
*/
uint well_aligned_int(__global ulong *_p, uint offset)
{
    __global char *p = (__global char *)_p;
    return *(__global uint *)(p + offset);
}

/*
** XOR a pair of Xi values computed at "round - 1" and store the result in the
** hash table being built for "round". Note that when building the table for
** even rounds we need to skip 1 padding byte present in the "round - 1" table
** (the "0xAB" byte mentioned in the description at the top of this file.) But
** also note we can't load data directly past this byte because this would
** cause an unaligned memory access which is undefined per the OpenCL spec.
**
** Return 0 if successfully stored, or 1 if the row overflowed.
*/
uint xor_and_store(uint round, __global char *ht_dst, uint row,
	uint slot_a, uint slot_b, __global ulong *a, __global ulong *b,
	__global uint *rowCounters ROUND_CNT_ARGS HT_DBG_ARGS)
{
    ulong xi0, xi1, xi2;
#if NR_ROWS_LOG >= 16 && NR_ROWS_LOG <= 20
    // Note: for NR_ROWS_LOG == 20, for odd rounds, we could optimize by not
    // storing the byte containing bits from the previous PREFIX block for
    if (round == 1 || round == 2)
      {
	// xor 24 bytes
	xi0 = *(a++) ^ *(b++);
	xi1 = *(a++) ^ *(b++);
	xi2 = *a ^ *b;
	if (round == 2)
	  {
	    // skip padding byte
	    xi0 = (xi0 >> 8) | (xi1 << (64 - 8));
	    xi1 = (xi1 >> 8) | (xi2 << (64 - 8));
	    xi2 = (xi2 >> 8);
	  }
      }
    else if (round == 3)
      {
	// xor 20 bytes
	xi0 = half_aligned_long(a, 0) ^ half_aligned_long(b, 0);
	xi1 = half_aligned_long(a, 8) ^ half_aligned_long(b, 8);
	xi2 = well_aligned_int(a, 16) ^ well_aligned_int(b, 16);
      }
    else if (round == 4 || round == 5)
      {
	// xor 16 bytes
	xi0 = half_aligned_long(a, 0) ^ half_aligned_long(b, 0);
	xi1 = half_aligned_long(a, 8) ^ half_aligned_long(b, 8);
	xi2 = 0;
	if (round == 4)
	  {
	    // skip padding byte
	    xi0 = (xi0 >> 8) | (xi1 << (64 - 8));
	    xi1 = (xi1 >> 8);
	  }
      }
    else if (round == 6)
      {
	// xor 12 bytes
	xi0 = *a++ ^ *b++;
	xi1 = *(__global uint *)a ^ *(__global uint *)b;
	xi2 = 0;
	if (round == 6)
	  {
	    // skip padding byte
	    xi0 = (xi0 >> 8) | (xi1 << (64 - 8));
	    xi1 = (xi1 >> 8);
	  }
      }
		else if (round == 7)
			{
				// xor 8 bytes
				xi0 = half_aligned_long(a, 0) ^ half_aligned_long(b, 0);
				xi1 = 0;
				xi2 = 0;
			}
    // invalid solutions (which start happenning in round 5) have duplicate
    // inputs and xor to zero, so discard them
	if (!xi0 && !xi1)
		return 0;

	/* Log xor non-zero event for diagnostics */
	/* Skip frequent xor_nonzero logging to prioritise stored-event samples */
	#ifdef DEBUG_EXTRACTION
	/* intentionally disabled: do not fill debug buffer with xor events */
	#endif
#else

#endif
	return ht_store(round, ht_dst, ENCODE_INPUTS(row, slot_a, slot_b),
		xi0, xi1, xi2, 0, rowCounters ROUND_CNT_PASS HT_DBG_PASS);
}

/*
** Execute one Equihash round. Read from ht_src, XOR colliding pairs of Xi,
** store them in ht_dst.
*/
void equihash_round(uint round,
	__global char *ht_src,
	__global char *ht_dst,
	__global uint *debug,
	__local uchar *first_words_data,
	__local uint *collisionsData,
	__local uint *collisionsNum,
	__global uint *rowCountersSrc,
	__global uint *rowCountersDst ROUND_CNT_ARGS HT_DBG_ARGS)
{
	/* Debug: mark that this round kernel executed (helps detect arg/binding issues) */
	atomic_inc(round_collisions + round);
    uint		tid = get_global_id(0);
    uint		tlid = get_local_id(0);
    __global char	*p;
    uint		cnt;
    __local uchar	*first_words = &first_words_data[(NR_SLOTS+2)*tlid];
    uchar		mask;
    uint		i, j;
    // NR_SLOTS is already oversized (by a factor of OVERHEAD), but we want to
    // make it even larger
    uint		n;
    uint		dropped_coll = 0;
    uint		dropped_stor = 0;
    __global ulong	*a, *b;
    uint		xi_offset;
    // read first words of Xi from the previous (round - 1) hash table
    xi_offset = xi_offset_for_round(round - 1);
    // the mask is also computed to read data from the previous round
#if NR_ROWS_LOG == 16
    mask = ((!(round % 2)) ? 0x0f : 0xf0);
#elif NR_ROWS_LOG == 18
    mask = ((!(round % 2)) ? 0x03 : 0x30);
#elif NR_ROWS_LOG == 19
    mask = ((!(round % 2)) ? 0x01 : 0x10);
#elif NR_ROWS_LOG == 20
    mask = ((!(round % 2)) ? 0xF0 : 0x0F);
#else

#endif
    uint thCollNum = 0;
    *collisionsNum = 0;
    barrier(CLK_LOCAL_MEM_FENCE);
    p = (ht_src + tid * NR_SLOTS * SLOT_LEN);
    uint rowIdx = tid/ROWS_PER_UINT;
    uint rowOffset = BITS_PER_ROW*(tid%ROWS_PER_UINT);
    cnt = (rowCountersSrc[rowIdx] >> rowOffset) & ROW_MASK;
    cnt = min(cnt, (uint)NR_SLOTS); // handle possible overflow in prev. round
    if (!cnt)
	// no elements in row, no collisions
	goto part2;
    p += xi_offset;
    for (i = 0; i < cnt; i++, p += SLOT_LEN)
	first_words[i] = (*(__global uchar *)p) & mask;
    // find collisions
    for (i = 0; i < cnt-1 && thCollNum < COLL_DATA_SIZE_PER_TH; i++)
      {
	uchar data_i = first_words[i];
	uint collision = (tid << 10) | (i << 5) | (i + 1);
	for (j = i+1; (j+4) < cnt;)
	  {
	      {
		uint isColl = ((data_i == first_words[j]) ? 1 : 0);
		if (isColl)
		  {
		    thCollNum++;
		    uint index = atomic_inc(collisionsNum);
		    collisionsData[index] = collision;
		  }
			collision++;
			j++;
		}
		{
		uint isColl = ((data_i == first_words[j]) ? 1 : 0);
		if (isColl)
		  {
		    thCollNum++;
		    uint index = atomic_inc(collisionsNum);
		    collisionsData[index] = collision;
		  }
		collision++;
		j++;
	      }
	      {
		uint isColl = ((data_i == first_words[j]) ? 1 : 0);
		if (isColl)
		  {
		    thCollNum++;
		    uint index = atomic_inc(collisionsNum);
		    collisionsData[index] = collision;
		  }
		collision++;
		j++;
	      }
	  }
	for (; j < cnt; j++)
	  {
	    uint isColl = ((data_i == first_words[j]) ? 1 : 0);
	    if (isColl)
	      {
		thCollNum++;
		uint index = atomic_inc(collisionsNum);
		collisionsData[index] = collision;
	      }
	    collision++;
	  }
      }

part2:
    barrier(CLK_LOCAL_MEM_FENCE);
	uint totalCollisions = *collisionsNum;
#ifdef PER_ROUND_COUNTS
	atomic_add(round_collisions + round, totalCollisions);
#endif
    for (uint index = tlid; index < totalCollisions; index += get_local_size(0))
      {
	uint collision = collisionsData[index];
	uint collisionThreadId = collision >> 10;
	uint i = (collision >> 5) & 0x1F;
	uint j = collision & 0x1F;
	__global uchar *ptr = ht_src + collisionThreadId * NR_SLOTS * SLOT_LEN +
	    xi_offset;
	a = (__global ulong *)(ptr + i * SLOT_LEN);
	b = (__global ulong *)(ptr + j * SLOT_LEN);
		dropped_stor += xor_and_store(round, ht_dst, collisionThreadId, i, j,
			a, b, rowCountersDst ROUND_CNT_PASS HT_DBG_PASS);
      }
#ifdef ENABLE_DEBUG
    debug[tid * 2] = dropped_coll;
    debug[tid * 2 + 1] = dropped_stor;
#endif
}

/*
** This defines kernel_round1, kernel_round2, ..., kernel_round7.
*/
#define KERNEL_ROUND(N) \
__kernel __attribute__((reqd_work_group_size(64, 1, 1))) \
void kernel_round ## N(__global char *ht_src, __global char *ht_dst, \
	__global uint *rowCountersSrc, __global uint *rowCountersDst, \
	__global uint *debug ROUND_CNT_ARGS HT_DBG_ARGS) \
{ \
	__local uchar first_words_data[(NR_SLOTS+2)*64]; \
	__local uint    collisionsData[COLL_DATA_SIZE_PER_TH * 64]; \
	__local uint    collisionsNum; \
	equihash_round(N, ht_src, ht_dst, debug, first_words_data, collisionsData, \
		&collisionsNum, rowCountersSrc, rowCountersDst ROUND_CNT_PASS HT_DBG_PASS); \
}
KERNEL_ROUND(1)
KERNEL_ROUND(2)
KERNEL_ROUND(3)
KERNEL_ROUND(4)
KERNEL_ROUND(5)
KERNEL_ROUND(6)

// kernel_round7 for 192,7 - final round takes an extra argument, "sols"
__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void kernel_round7(__global char *ht_src, __global char *ht_dst,
	__global uint *rowCountersSrc, __global uint *rowCountersDst,
	__global uint *debug, __global sols_t *sols ROUND_CNT_ARGS HT_DBG_ARGS)
{
	uint            tid = get_global_id(0);
	__local uchar   first_words_data[(NR_SLOTS+2)*64];
	__local uint    collisionsData[COLL_DATA_SIZE_PER_TH * 64];
	__local uint    collisionsNum;
	equihash_round(7, ht_src, ht_dst, debug, first_words_data, collisionsData,
		&collisionsNum, rowCountersSrc, rowCountersDst ROUND_CNT_PASS HT_DBG_PASS);
	if (!tid)
		sols->nr = sols->likely_invalids = 0;
}



uint expand_ref(__global char *ht, uint xi_offset, uint row, uint slot)
{
    return *(__global uint *)(ht + row * NR_SLOTS * SLOT_LEN +
	    slot * SLOT_LEN + xi_offset - 4);
}

/*
** Expand references to inputs. Return 1 if so far the solution appears valid,
** or 0 otherwise (an invalid solution would be a solution with duplicate
** inputs, which can be detected at the last step: round == 0).
*/
uint expand_refs(uint *ins, uint nr_inputs, __global char **htabs,
	uint round)
{
    __global char	*ht = htabs[round % 2];
    uint		i = nr_inputs - 1;
    uint		j = nr_inputs * 2 - 1;
    uint		xi_offset = xi_offset_for_round(round);
    int			dup_to_watch = -1;
    do
      {
	ins[j] = expand_ref(ht, xi_offset,
		DECODE_ROW(ins[i]), DECODE_SLOT1(ins[i]));
	ins[j - 1] = expand_ref(ht, xi_offset,
		DECODE_ROW(ins[i]), DECODE_SLOT0(ins[i]));
	if (!round)
	  {
	    // Temporarily revert to original logic to test if comprehensive check is the issue
	    if (dup_to_watch == -1)
		dup_to_watch = ins[j];
	    else if (ins[j] == dup_to_watch || ins[j - 1] == dup_to_watch)
		return 0;
	  }
	if (!i)
	    break ;
	i--;
	j -= 2;
      }
    while (1);
    return 1;
}

/*
** Verify if a potential solution is in fact valid.
*/
void potential_sol(__global char **htabs, __global sols_t *sols,
	uint ref0, uint ref1)
{
    uint	nr_values;
    uint	values_tmp[(1 << PARAM_K)];
    uint	sol_i;
    uint	i;
    nr_values = 0;
    values_tmp[nr_values++] = ref0;
    values_tmp[nr_values++] = ref1;
	// Start backtracking from PARAM_K-1 (round 6 for Equihash 192,7) 
	uint round = PARAM_K - 1;
    do
      {
	round--;
	if (!expand_refs(values_tmp, nr_values, htabs, round))
	    return ;
	nr_values *= 2;
      }
    while (round > 0);
    // solution appears valid, copy it to sols
    sol_i = atomic_inc(&sols->nr);
    if (sol_i >= MAX_SOLS)
	return ;
    for (i = 0; i < (1 << PARAM_K); i++)
	sols->values[sol_i][i] = values_tmp[i];
    sols->valid[sol_i] = 1;
}

/*
** Scan the hash tables to find Equihash solutions.
*/
__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void kernel_sols(__global char *ht0, __global char *ht1, __global sols_t *sols,
	__global uint *rowCountersSrc, __global uint *rowCountersDst, __global uint *potential_cnt)
{
    uint		tid = get_global_id(0);
    __global char	*htabs[2] = { ht0, ht1 };
    __global char	*hcounters[2] = { rowCountersSrc, rowCountersDst };
	uint		ht_i = (PARAM_K) % 2; // table filled at last round
    uint		cnt;
	uint		xi_offset = xi_offset_for_round(PARAM_K);
    uint		i, j;
    __global char	*a, *b;
    uint		ref_i, ref_j;
    // it's ok for the collisions array to be so small, as if it fills up
    // the potential solutions are likely invalid (many duplicate inputs)
    ulong		collisions;
    uint		coll;
#if NR_ROWS_LOG >= 16 && NR_ROWS_LOG <= 20
    // in the final hash table, we are looking for a match on both the bits
    // part of the previous PREFIX colliding bits, and the last PREFIX bits.
    uint		mask = 0xffffff;
#else

#endif
    a = htabs[ht_i] + tid * NR_SLOTS * SLOT_LEN;
    uint rowIdx = tid/ROWS_PER_UINT;
    uint rowOffset = BITS_PER_ROW*(tid%ROWS_PER_UINT);
    cnt = (rowCountersSrc[rowIdx] >> rowOffset) & ROW_MASK;
    cnt = min(cnt, (uint)NR_SLOTS); // handle possible overflow in last round
    coll = 0;
    a += xi_offset;
    for (i = 0; i < cnt; i++, a += SLOT_LEN)
      {
	uint a_data = ((*(__global uint *)a) & mask);
	ref_i = *(__global uint *)(a - 4);
	for (j = i + 1, b = a + SLOT_LEN; j < cnt; j++, b += SLOT_LEN)
	  {
	    if (a_data == ((*(__global uint *)b) & mask))
	      {
		/* increment global potential-match counter for diagnostics */
		atomic_inc(potential_cnt);
		ref_j = *(__global uint *)(b - 4);
		collisions = ((ulong)ref_i << 32) | ref_j;
		goto exit1;
	      }
	  }
      }
    return;

exit1:
    potential_sol(htabs, sols, collisions >> 32, collisions & 0xffffffff);
}

/*
** ============================================================================
** Tromp-Style Stage 1 Collision Detection (Equihash 192,7)
** ============================================================================
** 
** This kernel implements Tromp's bucket-based collision detection approach
** for the first stage. It replaces the original silentarmy round-based logic
** with a simpler bucket + slot design that correctly handles 24-bit collisions.
**
** Parameters:
**   NBUCKETS = 2^20 (1M buckets)
**   NSLOTS_STAGE1 = 96 slots per bucket
**   BUCKBITS = 20 (top 20 bits select bucket)
**   RESTBITS = 4 (bottom 4 bits for collision filtering)
**   DIGITBITS = 24 (total bits per stage)
**
** Input: Round 0 hashes (24 bytes each, from kernel_round0)
** Output: Stage 1 collision trees + slot counters
*/

#define RESTBITS 10                  // Collision filtering bits
#define BUCKBITS (24-RESTBITS)       // Bucket selection bits = 14
#define NBUCKETS_STAGE1 (1<<BUCKBITS) // 16K buckets (2^14)
#define NSLOTS_STAGE1 512             // Slots per bucket (increased for overflow)
#define HASHBYTES_STAGE0 24          // Round 0 hash size (192 bits / 8)
#define HASHBYTES_STAGE1 21          // Stage 1 hash size (24 - 3 bytes used for bucketing)

// Stage 1 slot structure
typedef struct {
    uint attr;                       // Tree attribution: bucketid + slot0 + slot1
    uchar hash[HASHBYTES_STAGE1];    // Remaining hash bytes (21 bytes)
} stage1_slot_t;

/**
 * kernel_stage1_collisions
 * 
 * Each work-item processes ONE bucket:
 * 1. Scan all Round 0 hashes to find hashes belonging to this bucket
 * 2. Within bucket, find collision pairs (matching 24-bit prefix)
 * 3. Store collision pairs in Stage 1 tree with XOR'd hash
 *
 * Global work size: NBUCKETS_STAGE1 (1M)
 * Local work size: 1 (simple single-threaded per bucket)
 */
__kernel
void kernel_stage1_collisions(
    __global uchar *hashes_round0,      // Input: Round 0 hashes (NHASHES * 24 bytes)
    __global stage1_slot_t *stage1_tree, // Output: Stage 1 collision trees
    __global uint *stage1_slot_counts,   // Output: Slot counts per bucket
    uint nhashes)                        // Number of Round 0 hashes to process
{
    uint bucketid = get_global_id(0);
    
    if (bucketid >= NBUCKETS_STAGE1)
        return;
    
    // Temporary storage for this bucket's hashes
    __private uint bucket_indices[NSLOTS_STAGE1];
    __private uchar bucket_restbits[NSLOTS_STAGE1];
    __private uint bucket_count = 0;
    
    // Step 1: Collect all hashes belonging to this bucket
    for (uint hidx = 0; hidx < nhashes && bucket_count < NSLOTS_STAGE1; hidx++) {
        __global uchar *hash = hashes_round0 + hidx * HASHBYTES_STAGE0;
        
        // Extract first 24 bits (3 bytes) for bucketing
        // Bits 0-19: bucket ID
        // Bits 20-23: RESTBITS for collision filtering
        uint bits24 = ((uint)hash[0] << 16) | ((uint)hash[1] << 8) | ((uint)hash[2]);
        uint hash_bucket = bits24 >> RESTBITS;  // Top 20 bits
        uint hash_rest = bits24 & ((1 << RESTBITS) - 1);           // Bottom 4 bits
        
        if (hash_bucket == bucketid) {
            bucket_indices[bucket_count] = hidx;
            bucket_restbits[bucket_count] = hash_rest;
            bucket_count++;
        }
    }
    
    // Step 2: Find collisions within bucket (pairs with matching RESTBITS)
    uint collision_count = 0;
    __global stage1_slot_t *output_base = stage1_tree + bucketid * NSLOTS_STAGE1;
    
    for (uint i = 0; i < bucket_count && collision_count < NSLOTS_STAGE1; i++) {
        for (uint j = i + 1; j < bucket_count && collision_count < NSLOTS_STAGE1; j++) {
            // Check if RESTBITS match (indicates 24-bit collision)
            if (bucket_restbits[i] != bucket_restbits[j])
                continue;
            
            // Found collision! Get the two hashes
            uint idx0 = bucket_indices[i];
            uint idx1 = bucket_indices[j];
            __global uchar *hash0 = hashes_round0 + idx0 * HASHBYTES_STAGE0;
            __global uchar *hash1 = hashes_round0 + idx1 * HASHBYTES_STAGE0;
            
            // Store collision in output
            __global stage1_slot_t *slot = &output_base[collision_count];
            
            // Encode tree attribution for Stage 1: 20-bit idx0 + 12-bit delta
            // Upper 20 bits: idx0 (supports up to 1M hashes = 500K nonces)
            // Lower 12 bits: (idx1 - idx0) & 0xFFF (delta, wraps if > 4095)
            uint delta = (idx1 - idx0) & 0xFFF;
            slot->attr = (idx0 << 12) | delta;
            
            // XOR the remaining hash bytes (skip first 3 bytes used for bucketing)
            // Store from hash byte 2 onwards (include the lower 4 bits of byte 2)
            for (uint b = 0; b < HASHBYTES_STAGE1; b++) {
                slot->hash[b] = hash0[b + 2] ^ hash1[b + 2];
            }
            
            collision_count++;
        }
    }
    
    // Store collision count for this bucket
    stage1_slot_counts[bucketid] = collision_count;
}


// Stage 2-6 slot structures (progressively smaller hashes)
#define HASHBYTES_STAGE2 18
#define HASHBYTES_STAGE3 15
#define HASHBYTES_STAGE4 12
#define HASHBYTES_STAGE5 9
#define HASHBYTES_STAGE6 6
#define HASHBYTES_STAGE7 3

typedef struct {
    uint attr;
    uchar hash[HASHBYTES_STAGE2];
} stage2_slot_t;

typedef struct {
    uint attr;
    uchar hash[HASHBYTES_STAGE3];
} stage3_slot_t;

typedef struct {
    uint attr;
    uchar hash[HASHBYTES_STAGE4];
} stage4_slot_t;

typedef struct {
    uint attr;
    uchar hash[HASHBYTES_STAGE5];
} stage5_slot_t;

typedef struct {
    uint attr;
    uchar hash[HASHBYTES_STAGE6];
} stage6_slot_t;

typedef struct {
    uint attr;
    uchar hash[HASHBYTES_STAGE7];
} stage7_slot_t;

/**
 * kernel_stage2_collisions
 * Stage 2: Process Stage 1 collisions
 */
__kernel
void kernel_stage2_collisions(
    __global stage1_slot_t *stage1_tree,
    __global uint *stage1_slot_counts,
    __global stage2_slot_t *stage2_tree,
    __global uint *stage2_slot_counts)
{
    uint bucketid = get_global_id(0);
    if (bucketid >= NBUCKETS_STAGE1) return;
    
    __private uint bucket_indices[NSLOTS_STAGE1];
    __private uchar bucket_restbits[NSLOTS_STAGE1];
    __private uint bucket_count = 0;
    
    // Collect Stage 1 collisions belonging to this bucket
    for (uint src_bucket = 0; src_bucket < NBUCKETS_STAGE1; src_bucket++) {
        uint nslots = stage1_slot_counts[src_bucket];
        if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
        
        __global stage1_slot_t *slots = stage1_tree + src_bucket * NSLOTS_STAGE1;
        
        for (uint s = 0; s < nslots && bucket_count < NSLOTS_STAGE1; s++) {
            __global uchar *hash = slots[s].hash;
            
            // Extract first 24 bits from XOR'd hash
            uint bits24 = ((uint)hash[0] << 16) | ((uint)hash[1] << 8) | ((uint)hash[2]);
            uint hash_bucket = bits24 >> RESTBITS;
            uint hash_rest = bits24 & ((1 << RESTBITS) - 1);
            
            if (hash_bucket == bucketid) {
                bucket_indices[bucket_count] = src_bucket * NSLOTS_STAGE1 + s;
                bucket_restbits[bucket_count] = hash_rest;
                bucket_count++;
            }
        }
    }
    
    // Find collisions
    uint collision_count = 0;
    __global stage2_slot_t *output_base = stage2_tree + bucketid * NSLOTS_STAGE1;
    
    for (uint i = 0; i < bucket_count && collision_count < NSLOTS_STAGE1; i++) {
        for (uint j = i + 1; j < bucket_count && collision_count < NSLOTS_STAGE1; j++) {
            if (bucket_restbits[i] != bucket_restbits[j]) continue;
            
            __global stage1_slot_t *slot0 = &stage1_tree[bucket_indices[i]];
            __global stage1_slot_t *slot1 = &stage1_tree[bucket_indices[j]];
            
            __global stage2_slot_t *out = &output_base[collision_count];
            // Store parent indices using 20+12 bit encoding like Stage 1
            // Upper 20 bits: idx0, Lower 12 bits: (idx1 - idx0) & 0xFFF
            uint delta = (bucket_indices[j] - bucket_indices[i]) & 0xFFF;
            out->attr = (bucket_indices[i] << 12) | delta;
            
            for (uint b = 0; b < HASHBYTES_STAGE2; b++) {
                out->hash[b] = slot0->hash[b + 2] ^ slot1->hash[b + 2];
            }
            
            collision_count++;
        }
    }
    
    stage2_slot_counts[bucketid] = collision_count;
    
    // Debug: Store bucket_count in high buckets to read back
    if (bucketid < 10) {
        stage2_slot_counts[NBUCKETS_STAGE1 - 100 + bucketid] = bucket_count;
    }
}

/**
 * kernel_stage3_collisions
 */
__kernel
void kernel_stage3_collisions(
    __global stage2_slot_t *stage2_tree,
    __global uint *stage2_slot_counts,
    __global stage3_slot_t *stage3_tree,
    __global uint *stage3_slot_counts)
{
    uint bucketid = get_global_id(0);
    if (bucketid >= NBUCKETS_STAGE1) return;
    
    __private uint bucket_indices[NSLOTS_STAGE1];
    __private uchar bucket_restbits[NSLOTS_STAGE1];
    __private uint bucket_count = 0;
    
    for (uint src_bucket = 0; src_bucket < NBUCKETS_STAGE1; src_bucket++) {
        uint nslots = stage2_slot_counts[src_bucket];
        if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
        
        __global stage2_slot_t *slots = stage2_tree + src_bucket * NSLOTS_STAGE1;
        
        for (uint s = 0; s < nslots && bucket_count < NSLOTS_STAGE1; s++) {
            __global uchar *hash = slots[s].hash;
            
            uint bits24 = ((uint)hash[0] << 16) | ((uint)hash[1] << 8) | ((uint)hash[2]);
            uint hash_bucket = bits24 >> RESTBITS;
            uint hash_rest = bits24 & ((1 << RESTBITS) - 1);
            
            if (hash_bucket == bucketid) {
                bucket_indices[bucket_count] = src_bucket * NSLOTS_STAGE1 + s;
                bucket_restbits[bucket_count] = hash_rest;
                bucket_count++;
            }
        }
    }
    
    uint collision_count = 0;
    __global stage3_slot_t *output_base = stage3_tree + bucketid * NSLOTS_STAGE1;
    
    for (uint i = 0; i < bucket_count && collision_count < NSLOTS_STAGE1; i++) {
        for (uint j = i + 1; j < bucket_count && collision_count < NSLOTS_STAGE1; j++) {
            if (bucket_restbits[i] != bucket_restbits[j]) continue;
            
            __global stage2_slot_t *slot0 = &stage2_tree[bucket_indices[i]];
            __global stage2_slot_t *slot1 = &stage2_tree[bucket_indices[j]];
            
            __global stage3_slot_t *out = &output_base[collision_count];
            // Store parent indices using 20+12 bit encoding
            uint delta = (bucket_indices[j] - bucket_indices[i]) & 0xFFF;
            out->attr = (bucket_indices[i] << 12) | delta;
            
            for (uint b = 0; b < HASHBYTES_STAGE3; b++) {
                out->hash[b] = slot0->hash[b + 2] ^ slot1->hash[b + 2];
            }
            
            collision_count++;
        }
    }
    
    stage3_slot_counts[bucketid] = collision_count;
}

/**
 * kernel_stage4_collisions
 */
__kernel
void kernel_stage4_collisions(
    __global stage3_slot_t *stage3_tree,
    __global uint *stage3_slot_counts,
    __global stage4_slot_t *stage4_tree,
    __global uint *stage4_slot_counts)
{
    uint bucketid = get_global_id(0);
    if (bucketid >= NBUCKETS_STAGE1) return;
    
    __private uint bucket_indices[NSLOTS_STAGE1];
    __private uchar bucket_restbits[NSLOTS_STAGE1];
    __private uint bucket_count = 0;
    
    for (uint src_bucket = 0; src_bucket < NBUCKETS_STAGE1; src_bucket++) {
        uint nslots = stage3_slot_counts[src_bucket];
        if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
        
        __global stage3_slot_t *slots = stage3_tree + src_bucket * NSLOTS_STAGE1;
        
        for (uint s = 0; s < nslots && bucket_count < NSLOTS_STAGE1; s++) {
            __global uchar *hash = slots[s].hash;
            
            uint bits24 = ((uint)hash[0] << 16) | ((uint)hash[1] << 8) | ((uint)hash[2]);
            uint hash_bucket = bits24 >> RESTBITS;
            uint hash_rest = bits24 & ((1 << RESTBITS) - 1);
            
            if (hash_bucket == bucketid) {
                bucket_indices[bucket_count] = src_bucket * NSLOTS_STAGE1 + s;
                bucket_restbits[bucket_count] = hash_rest;
                bucket_count++;
            }
        }
    }
    
    uint collision_count = 0;
    __global stage4_slot_t *output_base = stage4_tree + bucketid * NSLOTS_STAGE1;
    
    for (uint i = 0; i < bucket_count && collision_count < NSLOTS_STAGE1; i++) {
        for (uint j = i + 1; j < bucket_count && collision_count < NSLOTS_STAGE1; j++) {
            if (bucket_restbits[i] != bucket_restbits[j]) continue;
            
            __global stage3_slot_t *slot0 = &stage3_tree[bucket_indices[i]];
            __global stage3_slot_t *slot1 = &stage3_tree[bucket_indices[j]];
            
            __global stage4_slot_t *out = &output_base[collision_count];
            // Store parent indices using 20+12 bit encoding
            uint delta = (bucket_indices[j] - bucket_indices[i]) & 0xFFF;
            out->attr = (bucket_indices[i] << 12) | delta;
            
            for (uint b = 0; b < HASHBYTES_STAGE4; b++) {
                out->hash[b] = slot0->hash[b + 2] ^ slot1->hash[b + 2];
            }
            
            collision_count++;
        }
    }
    
    stage4_slot_counts[bucketid] = collision_count;
}

/**
 * kernel_stage5_collisions
 */
__kernel
void kernel_stage5_collisions(
    __global stage4_slot_t *stage4_tree,
    __global uint *stage4_slot_counts,
    __global stage5_slot_t *stage5_tree,
    __global uint *stage5_slot_counts)
{
    uint bucketid = get_global_id(0);
    if (bucketid >= NBUCKETS_STAGE1) return;
    
    __private uint bucket_indices[NSLOTS_STAGE1];
    __private uchar bucket_restbits[NSLOTS_STAGE1];
    __private uint bucket_count = 0;
    
    for (uint src_bucket = 0; src_bucket < NBUCKETS_STAGE1; src_bucket++) {
        uint nslots = stage4_slot_counts[src_bucket];
        if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
        
        __global stage4_slot_t *slots = stage4_tree + src_bucket * NSLOTS_STAGE1;
        
        for (uint s = 0; s < nslots && bucket_count < NSLOTS_STAGE1; s++) {
            __global uchar *hash = slots[s].hash;
            
            uint bits24 = ((uint)hash[0] << 16) | ((uint)hash[1] << 8) | ((uint)hash[2]);
            uint hash_bucket = bits24 >> RESTBITS;
            uint hash_rest = bits24 & ((1 << RESTBITS) - 1);
            
            if (hash_bucket == bucketid) {
                bucket_indices[bucket_count] = src_bucket * NSLOTS_STAGE1 + s;
                bucket_restbits[bucket_count] = hash_rest;
                bucket_count++;
            }
        }
    }
    
    uint collision_count = 0;
    __global stage5_slot_t *output_base = stage5_tree + bucketid * NSLOTS_STAGE1;
    
    for (uint i = 0; i < bucket_count && collision_count < NSLOTS_STAGE1; i++) {
        for (uint j = i + 1; j < bucket_count && collision_count < NSLOTS_STAGE1; j++) {
            if (bucket_restbits[i] != bucket_restbits[j]) continue;
            
            __global stage4_slot_t *slot0 = &stage4_tree[bucket_indices[i]];
            __global stage4_slot_t *slot1 = &stage4_tree[bucket_indices[j]];
            
            __global stage5_slot_t *out = &output_base[collision_count];
            // Store parent indices using 20+12 bit encoding
            uint delta = (bucket_indices[j] - bucket_indices[i]) & 0xFFF;
            out->attr = (bucket_indices[i] << 12) | delta;
            
            for (uint b = 0; b < HASHBYTES_STAGE5; b++) {
                out->hash[b] = slot0->hash[b + 2] ^ slot1->hash[b + 2];
            }
            
            collision_count++;
        }
    }
    
    stage5_slot_counts[bucketid] = collision_count;
}

/**
 * kernel_stage6_collisions
 */
__kernel
void kernel_stage6_collisions(
    __global stage5_slot_t *stage5_tree,
    __global uint *stage5_slot_counts,
    __global stage6_slot_t *stage6_tree,
    __global uint *stage6_slot_counts)
{
    uint bucketid = get_global_id(0);
    if (bucketid >= NBUCKETS_STAGE1) return;
    
    __private uint bucket_indices[NSLOTS_STAGE1];
    __private uchar bucket_restbits[NSLOTS_STAGE1];
    __private uint bucket_count = 0;
    
    for (uint src_bucket = 0; src_bucket < NBUCKETS_STAGE1; src_bucket++) {
        uint nslots = stage5_slot_counts[src_bucket];
        if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
        
        __global stage5_slot_t *slots = stage5_tree + src_bucket * NSLOTS_STAGE1;
        
        for (uint s = 0; s < nslots && bucket_count < NSLOTS_STAGE1; s++) {
            __global uchar *hash = slots[s].hash;
            
            uint bits24 = ((uint)hash[0] << 16) | ((uint)hash[1] << 8) | ((uint)hash[2]);
            uint hash_bucket = bits24 >> RESTBITS;
            uint hash_rest = bits24 & ((1 << RESTBITS) - 1);
            
            if (hash_bucket == bucketid) {
                bucket_indices[bucket_count] = src_bucket * NSLOTS_STAGE1 + s;
                bucket_restbits[bucket_count] = hash_rest;
                bucket_count++;
            }
        }
    }
    
    uint collision_count = 0;
    __global stage6_slot_t *output_base = stage6_tree + bucketid * NSLOTS_STAGE1;
    
    for (uint i = 0; i < bucket_count && collision_count < NSLOTS_STAGE1; i++) {
        for (uint j = i + 1; j < bucket_count && collision_count < NSLOTS_STAGE1; j++) {
            if (bucket_restbits[i] != bucket_restbits[j]) continue;
            
            __global stage5_slot_t *slot0 = &stage5_tree[bucket_indices[i]];
            __global stage5_slot_t *slot1 = &stage5_tree[bucket_indices[j]];
            
            __global stage6_slot_t *out = &output_base[collision_count];
            // Store parent indices using 20+12 bit encoding
            uint delta = (bucket_indices[j] - bucket_indices[i]) & 0xFFF;
            out->attr = (bucket_indices[i] << 12) | delta;
            
            for (uint b = 0; b < HASHBYTES_STAGE6; b++) {
                out->hash[b] = slot0->hash[b + 2] ^ slot1->hash[b + 2];
            }
            
            collision_count++;
        }
    }
    
    stage6_slot_counts[bucketid] = collision_count;
}

/**
 * kernel_stage7_collisions
 * Final stage - produces solution candidates
 */
__kernel
void kernel_stage7_collisions(
    __global stage6_slot_t *stage6_tree,
    __global uint *stage6_slot_counts,
    __global stage7_slot_t *stage7_tree,
    __global uint *stage7_slot_counts)
{
    uint bucketid = get_global_id(0);
    if (bucketid >= NBUCKETS_STAGE1) return;
    
    __private uint bucket_indices[NSLOTS_STAGE1];
    __private uchar bucket_restbits[NSLOTS_STAGE1];
    __private uint bucket_count = 0;
    
    for (uint src_bucket = 0; src_bucket < NBUCKETS_STAGE1; src_bucket++) {
        uint nslots = stage6_slot_counts[src_bucket];
        if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
        
        __global stage6_slot_t *slots = stage6_tree + src_bucket * NSLOTS_STAGE1;
        
        for (uint s = 0; s < nslots && bucket_count < NSLOTS_STAGE1; s++) {
            __global uchar *hash = slots[s].hash;
            
            uint bits24 = ((uint)hash[0] << 16) | ((uint)hash[1] << 8) | ((uint)hash[2]);
            uint hash_bucket = bits24 >> RESTBITS;
            uint hash_rest = bits24 & ((1 << RESTBITS) - 1);
            
            if (hash_bucket == bucketid) {
                bucket_indices[bucket_count] = src_bucket * NSLOTS_STAGE1 + s;
                bucket_restbits[bucket_count] = hash_rest;
                bucket_count++;
            }
        }
    }
    
    uint collision_count = 0;
    __global stage7_slot_t *output_base = stage7_tree + bucketid * NSLOTS_STAGE1;
    
    for (uint i = 0; i < bucket_count && collision_count < NSLOTS_STAGE1; i++) {
        for (uint j = i + 1; j < bucket_count && collision_count < NSLOTS_STAGE1; j++) {
            if (bucket_restbits[i] != bucket_restbits[j]) continue;
            
            __global stage6_slot_t *slot0 = &stage6_tree[bucket_indices[i]];
            __global stage6_slot_t *slot1 = &stage6_tree[bucket_indices[j]];
            
            // Final stage: verify full XOR is zero (all 24 bits)
            bool is_zero = true;
            for (uint b = 0; b < HASHBYTES_STAGE6; b++) {
                if ((slot0->hash[b + 2] ^ slot1->hash[b + 2]) != 0) {
                    is_zero = false;
                    break;
                }
            }
            
            if (!is_zero) continue;  // Not a valid solution
            
            __global stage7_slot_t *out = &output_base[collision_count];
            // Store parent indices using 20+12 bit encoding
            uint delta = (bucket_indices[j] - bucket_indices[i]) & 0xFFF;
            out->attr = (bucket_indices[i] << 12) | delta;
            
            // Final hash should be all zeros
            for (uint b = 0; b < HASHBYTES_STAGE7; b++) {
                out->hash[b] = 0;
            }
            
            collision_count++;
        }
    }
    
    stage7_slot_counts[bucketid] = collision_count;
}

