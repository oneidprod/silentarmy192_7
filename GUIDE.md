Project Goal: Port SilentArmy to Equihash 192,7 for Intel UHD 630
1. Mathematical Logic and Bit Progression
• The 24-bit Reduction: The miner must transition from 20-bit or 27-bit logic to a strict 24-bit (3-byte) reduction per round. The state progression must follow: 192 → 168 → 144 → 120 → 96 → 72 → 48 → 24.
• Xi Formulas: In input.cl, set the bit reduction formula to xi = (192 - 24 * i - NR_ROWS_LOG) / 8.0. Update the round-by-round byte storage counts to 22, 19, 16, 13, 10, 7, 4 bytes respectively.
• Collision Masks: For 20-bit rows (NR_ROWS_LOG 20), implement a 4-bit nibble mask: mask = ((!(round % 2)) ? 0xF0 : 0x0F). Use a 24-bit mask (0xffffff) for final solution collisions.
2. OpenCL Kernel Fixes (input.cl)
• Sequential Word Mapping (Round 0): Implement sequential mapping for the initial hash. Because 24 bytes align perfectly with 8-byte ulong words, Index 1 should map to words 0, 1, and 2, while Index 2 maps to words 3, 4, and 5. No fractional bit-shifting is required for 192,7.
• Round Structure: Equihash 192,7 requires exactly 7 rounds (0–6). Remove all references to Rounds 7 or 8.
• Custom Final Round: The macro-generated KERNEL_ROUND(6) must be replaced with a custom 6-argument kernel_round6 that includes a __global sols_t *sols pointer to properly extract solutions.
• Array Sizes: Ensure the v array used for Blake2b mixing is increased to 16 elements to prevent memory overflows.
3. Host Logic Fixes (main.c)
• Solution Prefix: The pool expects a 400-byte solution. Prepend the hex solution string with the CompactSize VarInt fd9001. This is critical to resolve Stratum Error 25 (Invalid Share).
• Safe Buffer Management: In s_hexdump, declare static char buf as a large array (e.g., buf) instead of a single character to prevent immediate crashes when printing solutions.
• Heap Allocation: In the solution verification logic, allocate the seen buffer on the heap using malloc rather than the stack to prevent memory corruption on Intel systems.
• Argument Mismatch: Ensure every clSetKernelArg call passes 4 parameters, specifically including sizeof(cl_mem) for every buffer passed to the GPU.
4. Parameter and Infrastructure Definitions (param.h)
• Type Definitions: Explicitly add typedef uint8_t uchar; and typedef uint32_t uint; to resolve "unknown type" compiler errors.
• Offset Logic: Redefine the xi_offset_for_round(round) macro to follow the 3-byte progression: (8 + (round * 3)).
• Intel Optimization: Lock the configuration to NR_ROWS_LOG 20 and OVERHEAD 1 to fit within the 2.1GB VRAM limit of the Beignet OpenCL driver for Intel UHD 630.
• Personalization: Ensure the BLAKE2b personalization string is updated to ZERO_PoW with N=192 and K=7 encoded as little-endian integers.
Execution Instructions for Copilot
• Remove all Equihash 200,9 remnants including 25-byte segment logic and 9-round loop structures.
• Synchronize the bit-alignment between ht_store and xor_and_store to ensure the GPU is not reading "garbage" data between rounds.
• Implement a 7-iteration solver loop in the host code to match the K=7 parameter set.