/* BUILD_ID: 20260306_083000_force_recompile_bucket_slot_encoding */
#include "param.h"

#define KERNEL_BUILD_ID 20260306083000UL  // Force Beignet recompilation

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
	/* eq1927/Tromp block 2 layout: m[0]=nonce (low 32), m[1]=index<<32 (high 32) */
	ulong word0 = blake_state[8]; /* nonce stored in extra slot [8] by host */
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
	// 140-byte headernonce + 4-byte index = 144
	v[12] ^= 144;
	// last block
	v[14] ^= (ulong)-1;

	// round 1 — sigma[0]: m[0] at col0 x, m[1] at col0 y
	mix(v[0], v[4], v[8],  v[12], word0, word1);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 2 — sigma[1]: m[1] at col4 x, m[0] at col5 x
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], word1, 0);
	mix(v[1], v[6], v[11], v[12], word0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 3 — sigma[2]: m[0] at col1 y, m[1] at col6 y
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, word0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, word1);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 4 — sigma[3]: m[1] at col1 y, m[0] at col6 y
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, word1);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, word0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 5 — sigma[4]: m[0] at col0 y, m[1] at col4 y
	mix(v[0], v[4], v[8],  v[12], 0, word0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, word1);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 6 — sigma[5]: m[0] at col2 x, m[1] at col7 y
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], word0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, word1);
	// round 7 — sigma[6]: m[1] at col1 x, m[0] at col4 x
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], word1, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], word0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 8 — sigma[7]: m[1] at col2 y, m[0] at col4 y
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, word1);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, word0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 9 — sigma[8]: m[0] at col3 x, m[1] at col6 x
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], word0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], word1, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 10 — sigma[9]: m[1] at col3 x, m[0] at col7 y
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], word1, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, word0);
	// round 11 — sigma[10]=sigma[0]: same as round 1
	mix(v[0], v[4], v[8],  v[12], word0, word1);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], 0, 0);
	mix(v[1], v[6], v[11], v[12], 0, 0);
	mix(v[2], v[7], v[8],  v[13], 0, 0);
	mix(v[3], v[4], v[9],  v[14], 0, 0);
	// round 12 — sigma[11]=sigma[1]: same as round 2
	mix(v[0], v[4], v[8],  v[12], 0, 0);
	mix(v[1], v[5], v[9],  v[13], 0, 0);
	mix(v[2], v[6], v[10], v[14], 0, 0);
	mix(v[3], v[7], v[11], v[15], 0, 0);
	mix(v[0], v[5], v[10], v[15], word1, 0);
	mix(v[1], v[6], v[11], v[12], word0, 0);
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

// =============================================================================
// Stage slot structures (source-bucket architecture)
// =============================================================================

/* Stage 0: raw BLAKE2b output — defined here so Stage 1 kernel can reference it */
typedef struct {
    uint  attr;      /* hash index (xi): 0..2^25-1 */
    uchar hash[24];  /* raw 24-byte hash */
} stage0_slot_t;

/* Collision detection constants */
#define RESTBITS        4
#define BUCKBITS        (24 - RESTBITS)
#define NBUCKETS_STAGE1 (1 << BUCKBITS)   /* 2^19 = 512K */
#define NSLOTS_STAGE1   64

/* Hash widths at each stage (bytes remaining after XOR cancellation) */
#define HASHBYTES_STAGE0 24
#define HASHBYTES_STAGE1 21
#define HASHBYTES_STAGE2 18
#define HASHBYTES_STAGE3 15
#define HASHBYTES_STAGE4 12
#define HASHBYTES_STAGE5  9
#define HASHBYTES_STAGE6  6
#define HASHBYTES_STAGE7  3

/* Stage 1-7 slot structs.
   attr = (src_bucket << 12) | (slot_i << 6) | slot_j  (20+6+6 = 32 bits)  */
typedef struct { uint attr; uchar hash[21]; uchar pad[3]; } stage1_slot_t;
typedef struct { uint attr; uchar hash[18]; } stage2_slot_t;
typedef struct { uint attr; uchar hash[15]; } stage3_slot_t;
typedef struct { uint attr; uchar hash[12]; } stage4_slot_t;
typedef struct { uint attr; uchar hash[9];  } stage5_slot_t;
typedef struct { uint attr; uchar hash[6];  } stage6_slot_t;
typedef struct { uint attr; uchar hash[3];  } stage7_slot_t;

// =============================================================================
// Stage kernels — source-bucket-per-work-item pattern
//
// Each work item owns ONE source bucket.  It reads NSLOTS_STAGE1 input slots,
// finds collision pairs (matching RESTBITS = bottom 4 bits of hash[2]),
// XORs hashes starting at byte 3, and atomically writes results to the output
// bucket = top BUCKBITS bits of the XOR'd hash bytes 0-2.
//
// attr = (src_bucket << 12) | (slot_i << 6) | slot_j  (uniform, all stages)
// =============================================================================

/** kernel_stage1_collisions
 *  Input:  tree0 (stage0_slot_t) — GPU-generated BLAKE2b hashes
 *  Output: tree1 (stage1_slot_t)
 */
__kernel
void kernel_stage1_collisions(
    __global stage0_slot_t *tree0,
    __global uint          *tree0_counts,
    __global stage1_slot_t *tree1,
    __global uint          *tree1_counts)
{
    uint src = get_global_id(0);
    if (src >= NBUCKETS_STAGE1) return;

    uint nslots = tree0_counts[src];
    if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
    __global stage0_slot_t *in = tree0 + src * NSLOTS_STAGE1;

    for (uint i = 0; i < nslots; i++) {
        uint ri = in[i].hash[2] & ((1u << RESTBITS) - 1u);
        for (uint j = i + 1; j < nslots; j++) {
            if ((in[j].hash[2] & ((1u << RESTBITS) - 1u)) != ri) continue;

            uchar xh[21];
            for (uint b = 0; b < 21; b++)
                xh[b] = in[i].hash[b + 3] ^ in[j].hash[b + 3];

            { uint nz = 0; for (uint b = 0; b < 21; b++) nz |= xh[b]; if (!nz) continue; }

            uint bits24 = ((uint)xh[0] << 16) | ((uint)xh[1] << 8) | xh[2];
            uint ob = bits24 >> RESTBITS;
            uint sl = atomic_inc(&tree1_counts[ob]);
            if (sl >= NSLOTS_STAGE1) continue;

            __global stage1_slot_t *out = tree1 + ob * NSLOTS_STAGE1 + sl;
            out->attr = (src << 12) | (i << 6) | j;
            for (uint b = 0; b < 21; b++)
                out->hash[b] = xh[b];
        }
    }
}

/** kernel_stage2_collisions
 *  Input:  tree1 (stage1_slot_t, 21-byte hash)
 *  Output: tree2 (stage2_slot_t, 18-byte hash)
 */
__kernel
void kernel_stage2_collisions(
    __global stage1_slot_t *tree1,
    __global uint          *tree1_counts,
    __global stage2_slot_t *tree2,
    __global uint          *tree2_counts)
{
    uint src = get_global_id(0);
    if (src >= NBUCKETS_STAGE1) return;

    uint nslots = tree1_counts[src];
    if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
    __global stage1_slot_t *in = tree1 + src * NSLOTS_STAGE1;

    for (uint i = 0; i < nslots; i++) {
        uint ri = in[i].hash[2] & ((1u << RESTBITS) - 1u);
        for (uint j = i + 1; j < nslots; j++) {
            if ((in[j].hash[2] & ((1u << RESTBITS) - 1u)) != ri) continue;

            uchar xh[18];
            for (uint b = 0; b < 18; b++)
                xh[b] = in[i].hash[b + 3] ^ in[j].hash[b + 3];

            { uint nz = 0; for (uint b = 0; b < 18; b++) nz |= xh[b]; if (!nz) continue; }

            uint bits24 = ((uint)xh[0] << 16) | ((uint)xh[1] << 8) | xh[2];
            uint ob = bits24 >> RESTBITS;
            uint sl = atomic_inc(&tree2_counts[ob]);
            if (sl >= NSLOTS_STAGE1) continue;

            __global stage2_slot_t *out = tree2 + ob * NSLOTS_STAGE1 + sl;
            out->attr = (src << 12) | (i << 6) | j;
            for (uint b = 0; b < 18; b++)
                out->hash[b] = xh[b];
        }
    }
}

/** kernel_stage3_collisions
 *  Input:  tree2 (stage2_slot_t, 18-byte hash)
 *  Output: tree3 (stage3_slot_t, 15-byte hash)
 */
__kernel
void kernel_stage3_collisions(
    __global stage2_slot_t *tree2,
    __global uint          *tree2_counts,
    __global stage3_slot_t *tree3,
    __global uint          *tree3_counts)
{
    uint src = get_global_id(0);
    if (src >= NBUCKETS_STAGE1) return;

    uint nslots = tree2_counts[src];
    if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
    __global stage2_slot_t *in = tree2 + src * NSLOTS_STAGE1;

    for (uint i = 0; i < nslots; i++) {
        uint ri = in[i].hash[2] & ((1u << RESTBITS) - 1u);
        for (uint j = i + 1; j < nslots; j++) {
            if ((in[j].hash[2] & ((1u << RESTBITS) - 1u)) != ri) continue;

            uchar xh[15];
            for (uint b = 0; b < 15; b++)
                xh[b] = in[i].hash[b + 3] ^ in[j].hash[b + 3];

            { uint nz = 0; for (uint b = 0; b < 15; b++) nz |= xh[b]; if (!nz) continue; }

            uint bits24 = ((uint)xh[0] << 16) | ((uint)xh[1] << 8) | xh[2];
            uint ob = bits24 >> RESTBITS;
            uint sl = atomic_inc(&tree3_counts[ob]);
            if (sl >= NSLOTS_STAGE1) continue;

            __global stage3_slot_t *out = tree3 + ob * NSLOTS_STAGE1 + sl;
            out->attr = (src << 12) | (i << 6) | j;
            for (uint b = 0; b < 15; b++)
                out->hash[b] = xh[b];
        }
    }
}

/** kernel_stage4_collisions
 *  Input:  tree3 (stage3_slot_t, 15-byte hash)
 *  Output: tree4 (stage4_slot_t, 12-byte hash)
 */
__kernel
void kernel_stage4_collisions(
    __global stage3_slot_t *tree3,
    __global uint          *tree3_counts,
    __global stage4_slot_t *tree4,
    __global uint          *tree4_counts)
{
    uint src = get_global_id(0);
    if (src >= NBUCKETS_STAGE1) return;

    uint nslots = tree3_counts[src];
    if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
    __global stage3_slot_t *in = tree3 + src * NSLOTS_STAGE1;

    for (uint i = 0; i < nslots; i++) {
        uint ri = in[i].hash[2] & ((1u << RESTBITS) - 1u);
        for (uint j = i + 1; j < nslots; j++) {
            if ((in[j].hash[2] & ((1u << RESTBITS) - 1u)) != ri) continue;

            uchar xh[12];
            for (uint b = 0; b < 12; b++)
                xh[b] = in[i].hash[b + 3] ^ in[j].hash[b + 3];

            { uint nz = 0; for (uint b = 0; b < 12; b++) nz |= xh[b]; if (!nz) continue; }

            uint bits24 = ((uint)xh[0] << 16) | ((uint)xh[1] << 8) | xh[2];
            uint ob = bits24 >> RESTBITS;
            uint sl = atomic_inc(&tree4_counts[ob]);
            if (sl >= NSLOTS_STAGE1) continue;

            __global stage4_slot_t *out = tree4 + ob * NSLOTS_STAGE1 + sl;
            out->attr = (src << 12) | (i << 6) | j;
            for (uint b = 0; b < 12; b++)
                out->hash[b] = xh[b];
        }
    }
}

/** kernel_stage5_collisions
 *  Input:  tree4 (stage4_slot_t, 12-byte hash)
 *  Output: tree5 (stage5_slot_t, 9-byte hash)
 */
__kernel
void kernel_stage5_collisions(
    __global stage4_slot_t *tree4,
    __global uint          *tree4_counts,
    __global stage5_slot_t *tree5,
    __global uint          *tree5_counts)
{
    uint src = get_global_id(0);
    if (src >= NBUCKETS_STAGE1) return;

    uint nslots = tree4_counts[src];
    if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
    __global stage4_slot_t *in = tree4 + src * NSLOTS_STAGE1;

    for (uint i = 0; i < nslots; i++) {
        uint ri = in[i].hash[2] & ((1u << RESTBITS) - 1u);
        for (uint j = i + 1; j < nslots; j++) {
            if ((in[j].hash[2] & ((1u << RESTBITS) - 1u)) != ri) continue;

            uchar xh[9];
            for (uint b = 0; b < 9; b++)
                xh[b] = in[i].hash[b + 3] ^ in[j].hash[b + 3];

            { uint nz = 0; for (uint b = 0; b < 9; b++) nz |= xh[b]; if (!nz) continue; }

            uint bits24 = ((uint)xh[0] << 16) | ((uint)xh[1] << 8) | xh[2];
            uint ob = bits24 >> RESTBITS;
            uint sl = atomic_inc(&tree5_counts[ob]);
            if (sl >= NSLOTS_STAGE1) continue;

            __global stage5_slot_t *out = tree5 + ob * NSLOTS_STAGE1 + sl;
            out->attr = (src << 12) | (i << 6) | j;
            for (uint b = 0; b < 9; b++)
                out->hash[b] = xh[b];
        }
    }
}

/** kernel_stage6_collisions
 *  Input:  tree5 (stage5_slot_t, 9-byte hash)
 *  Output: tree6 (stage6_slot_t, 6-byte hash)
 */
__kernel
void kernel_stage6_collisions(
    __global stage5_slot_t *tree5,
    __global uint          *tree5_counts,
    __global stage6_slot_t *tree6,
    __global uint          *tree6_counts)
{
    uint src = get_global_id(0);
    if (src >= NBUCKETS_STAGE1) return;

    uint nslots = tree5_counts[src];
    if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
    __global stage5_slot_t *in = tree5 + src * NSLOTS_STAGE1;

    for (uint i = 0; i < nslots; i++) {
        uint ri = in[i].hash[2] & ((1u << RESTBITS) - 1u);
        for (uint j = i + 1; j < nslots; j++) {
            if ((in[j].hash[2] & ((1u << RESTBITS) - 1u)) != ri) continue;

            uchar xh[6];
            for (uint b = 0; b < 6; b++)
                xh[b] = in[i].hash[b + 3] ^ in[j].hash[b + 3];

            { uint nz = 0; for (uint b = 0; b < 6; b++) nz |= xh[b]; if (!nz) continue; }

            uint bits24 = ((uint)xh[0] << 16) | ((uint)xh[1] << 8) | xh[2];
            uint ob = bits24 >> RESTBITS;
            uint sl = atomic_inc(&tree6_counts[ob]);
            if (sl >= NSLOTS_STAGE1) continue;

            __global stage6_slot_t *out = tree6 + ob * NSLOTS_STAGE1 + sl;
            out->attr = (src << 12) | (i << 6) | j;
            for (uint b = 0; b < 6; b++)
                out->hash[b] = xh[b];
        }
    }
}

/** kernel_stage7_collisions
 *  Input:  tree6 (stage6_slot_t, 6-byte hash)
 *  Output: tree7 (stage7_slot_t) — valid solutions only
 *  A valid solution requires matching restbits AND bytes 3-5 XOR = 000.
 *  All solutions route to bucket 0 (XOR of last 3 bytes = 0 → top bits = 0).
 */
__kernel
void kernel_stage7_collisions(
    __global stage6_slot_t *tree6,
    __global uint          *tree6_counts,
    __global stage7_slot_t *tree7,
    __global uint          *tree7_counts)
{
    uint src = get_global_id(0);
    if (src >= NBUCKETS_STAGE1) return;

    uint nslots = tree6_counts[src];
    if (nslots > NSLOTS_STAGE1) nslots = NSLOTS_STAGE1;
    __global stage6_slot_t *in = tree6 + src * NSLOTS_STAGE1;

    for (uint i = 0; i < nslots; i++) {
        uint ri = in[i].hash[2] & ((1u << RESTBITS) - 1u);
        for (uint j = i + 1; j < nslots; j++) {
            if ((in[j].hash[2] & ((1u << RESTBITS) - 1u)) != ri) continue;
            /* Final check: remaining 3 bytes must XOR to zero */
            if ((in[i].hash[3] ^ in[j].hash[3]) != 0) continue;
            if ((in[i].hash[4] ^ in[j].hash[4]) != 0) continue;
            if ((in[i].hash[5] ^ in[j].hash[5]) != 0) continue;

            /* Valid Equihash solution pair — store in bucket 0 */
            uint sl = atomic_inc(&tree7_counts[0]);
            if (sl >= 65536) continue;  /* generous cap: 65536 stage7 candidates max */

            __global stage7_slot_t *out = tree7 + sl;
            out->attr = (src << 12) | (i << 6) | j;
            out->hash[0] = out->hash[1] = out->hash[2] = 0;
        }
    }
}

// =============================================================================
// Stage 0: GPU hash generation (replaces CPU generate_round0_hashes)
// =============================================================================
/* stage0_slot_t is defined above (line ~1144) — no redefinition here */

/**
 * kernel_round0_gen
 *
 * Generates all 2^25 hashes for one mining nonce and writes them into
 * tree0, organized by bucket (top BUCKBITS bits of first 24 bits of hash).
 *
 * Global work size: 2^24 (each work item computes 2 hashes via one BLAKE2b call)
 * blake_state: 8-word BLAKE2b state after processing the block header
 */
__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void kernel_round0_gen(
    __global ulong *blake_state,       /* 8 x ulong pre-initialized state (after hdr block1) */
    __global stage0_slot_t *tree0,     /* NBUCKETS_STAGE1 * NSLOTS_STAGE1 slots */
    __global uint *tree0_counts,       /* atomic slot counters [NBUCKETS_STAGE1] */
    uint nonce)                        /* mining nonce (m[0] low 32 bits of block2) */
{
    uint i = get_global_id(0);
    /* Tromp/eq1927 standard block 2 layout (matches 140-byte headernonce convention):
     *   bytes 0-3:   nonce (from headernonce bytes 128-131) → m[0] low 32 bits
     *   bytes 4-11:  zeros
     *   bytes 12-15: htole32(i/2) = g → m[1] high 32 bits
     * block1 = 128 zero bytes (no nonce); block2 = 16 bytes; t=144 total. */
    ulong word0 = (ulong)nonce;        /* m[0] low32 = nonce */
    ulong word1 = (ulong)i << 32;      /* m[1] high32 = blake-call index g */

    ulong v[16];
    v[0]  = blake_state[0]; v[1]  = blake_state[1];
    v[2]  = blake_state[2]; v[3]  = blake_state[3];
    v[4]  = blake_state[4]; v[5]  = blake_state[5];
    v[6]  = blake_state[6]; v[7]  = blake_state[7];
    v[8]  = blake_iv[0];    v[9]  = blake_iv[1];
    v[10] = blake_iv[2];    v[11] = blake_iv[3];
    v[12] = blake_iv[4];    v[13] = blake_iv[5];
    v[14] = blake_iv[6];    v[15] = blake_iv[7];
    /* 128-byte headernonce block1 + 16-byte block2 (4 bytes g + 12 zero pad) = 144 total */
    v[12] ^= 144;
    v[14] ^= (ulong)-1;

    /* 12 BLAKE2b rounds using sigma table — matches CPU blake.c exactly.
     * m[] = {word0, word1, 0, ..., 0} (16 ulongs, m[0] and m[1] non-zero) */
    __constant uchar blake2b_sigma[12][16] = {
        {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
        { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
        { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
        {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
        {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
        {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
        { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
        { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
        {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
        { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 },
        {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
        { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
    };
    /* Helper: select m[0]=word0, m[1]=word1, m[2..15]=0 by sigma index */
    #define MSG(idx) ((idx) == 0 ? word0 : ((idx) == 1 ? word1 : (ulong)0))
    for (int r = 0; r < 12; r++) {
        __constant uchar *s = blake2b_sigma[r];
        mix(v[0], v[4], v[8],  v[12], MSG(s[0]),  MSG(s[1]));
        mix(v[1], v[5], v[9],  v[13], MSG(s[2]),  MSG(s[3]));
        mix(v[2], v[6], v[10], v[14], MSG(s[4]),  MSG(s[5]));
        mix(v[3], v[7], v[11], v[15], MSG(s[6]),  MSG(s[7]));
        mix(v[0], v[5], v[10], v[15], MSG(s[8]),  MSG(s[9]));
        mix(v[1], v[6], v[11], v[12], MSG(s[10]), MSG(s[11]));
        mix(v[2], v[7], v[8],  v[13], MSG(s[12]), MSG(s[13]));
        mix(v[3], v[4], v[9],  v[14], MSG(s[14]), MSG(s[15]));
    }
    #undef MSG

    /* Finalize: 48-byte BLAKE2b output as 6 ulongs (h0..h5) */
    ulong hh0 = blake_state[0] ^ v[0] ^ v[8];
    ulong hh1 = blake_state[1] ^ v[1] ^ v[9];
    ulong hh2 = blake_state[2] ^ v[2] ^ v[10];
    ulong hh3 = blake_state[3] ^ v[3] ^ v[11];
    ulong hh4 = blake_state[4] ^ v[4] ^ v[12];
    ulong hh5 = blake_state[5] ^ v[5] ^ v[13];

    /* Hash 0: bytes 0..23 from hh0, hh1, hh2 */
    {
        uint bits24 = (((uint)(hh0      ) & 0xFF) << 16) |
                      (((uint)(hh0 >>  8) & 0xFF) <<  8) |
                       ((uint)(hh0 >> 16) & 0xFF);
        uint bucket = bits24 >> RESTBITS;
        uint slot = atomic_inc(&tree0_counts[bucket]);
        if (slot < NSLOTS_STAGE1) {
            __global stage0_slot_t *s = tree0 + bucket * NSLOTS_STAGE1 + slot;
            s->attr = i * 2;
            *(__global uint *)(s->hash +  0) = (uint)hh0;
            *(__global uint *)(s->hash +  4) = (uint)(hh0 >> 32);
            *(__global uint *)(s->hash +  8) = (uint)hh1;
            *(__global uint *)(s->hash + 12) = (uint)(hh1 >> 32);
            *(__global uint *)(s->hash + 16) = (uint)hh2;
            *(__global uint *)(s->hash + 20) = (uint)(hh2 >> 32);
        }
    }

    /* Hash 1: bytes 24..47 from hh3, hh4, hh5 */
    {
        uint bits24 = (((uint)(hh3      ) & 0xFF) << 16) |
                      (((uint)(hh3 >>  8) & 0xFF) <<  8) |
                       ((uint)(hh3 >> 16) & 0xFF);
        uint bucket = bits24 >> RESTBITS;
        uint slot = atomic_inc(&tree0_counts[bucket]);
        if (slot < NSLOTS_STAGE1) {
            __global stage0_slot_t *s = tree0 + bucket * NSLOTS_STAGE1 + slot;
            s->attr = i * 2 + 1;
            *(__global uint *)(s->hash +  0) = (uint)hh3;
            *(__global uint *)(s->hash +  4) = (uint)(hh3 >> 32);
            *(__global uint *)(s->hash +  8) = (uint)hh4;
            *(__global uint *)(s->hash + 12) = (uint)(hh4 >> 32);
            *(__global uint *)(s->hash + 16) = (uint)hh5;
            *(__global uint *)(s->hash + 20) = (uint)(hh5 >> 32);
        }
    }
}

/* Extract just the attr (first uint32_t) from each slot in a tree buffer.
 * Used for low-memory attr readback: avoids malloc-ing the full slot-stride tmp buffer.
 * Input:  tree buffer (raw bytes, stride = slot_stride bytes per slot)
 * Output: compact uint array, one attr per slot */
__kernel void kernel_extract_attrs(
    __global const uchar *tree,
    uint slot_stride,
    __global uint *out,
    uint base_offset)
{
    size_t k = (size_t)get_global_id(0) + base_offset;
    out[k] = *((__global const uint *)(tree + k * slot_stride));
}
