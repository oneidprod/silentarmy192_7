#!/usr/bin/env python3
# Calculate xenoncat parameters for Equihash 192,7

N = 192
K = 7
BUCKET_BITS = 8
COLLISION_BITS = 8  # For 256 buckets with 12-bit collision search

print("Equihash 192,7 Parameter Calculation")
print("=" * 60)

for stage in range(K + 1):
    bits_remaining = N - stage * (N // K)
    bytes_exact = bits_remaining / 8
    bytes_rounded = int((bits_remaining + 7) // 8)
    bytes_aligned = ((bytes_rounded + 3) // 4) * 4
    
    # Calculate the byte offset for extracting collision bits
    # The pattern from 200,9: extract from near the end of the data
    # For State 0 with 192 bits (24 bytes): want bits [191:184] for bucket
    #   Then bits [183:176] for collision (8 bits)
    #   Bit 183 is in byte 22 (bits 183-176 = bytes 22-21)
    
    if stage < K:
        # After consuming stage*24 bits, we have bits_remaining left
        # Bucket bits are the highest BUCKET_BITS
        # Collision bits are the next COLLISION_BITS after bucket
        
        # Highest bit position (0-indexed from right)
        highest_bit = bits_remaining - 1
        
        # Bucket occupies bits [highest:highest-7] (8 bits)
        bucket_start_bit = highest_bit - BUCKET_BITS + 1
        
        # Collision bits start after bucket
        collision_start_bit = bucket_start_bit - 1
        collision_end_bit = collision_start_bit - COLLISION_BITS + 1
        
        # Which byte contains collision_start_bit?
        byte_offset = collision_start_bit // 8
        bit_in_byte = collision_start_bit % 8
        
        # If collision bits span byte boundary, need to read as word and shift
        if bit_in_byte >= COLLISION_BITS - 1:
            method = f"[{byte_offset}]"
        else:
            method = f"[{byte_offset}]>>{8 - COLLISION_BITS - bit_in_byte}"
    else:
        byte_offset = 0
        method = "N/A"
    
    print(f"\nState {stage}:")
    print(f"  Bits in Xorwork: {bits_remaining}")
    print(f"  Bytes (exact): {bytes_exact:.1f}")
    print(f"  Bytes (rounded): {bytes_rounded}")
    print(f"  Bytes (4-byte aligned): {bytes_aligned}")
    if stage < K:
        print(f"  Collision bits: [{collision_start_bit}:{collision_end_bit}]")
        print(f"  Byte offset to read: {byte_offset}")
        print(f"  Extraction method: {method}")

print("\n" + "=" * 60)
print("STATE_BYTES values:")
for stage in range(K + 1):
    bits_remaining = N - stage * (N // K)
    bytes_aligned = ((int((bits_remaining + 7) // 8) + 3) // 4) * 4
    print(f"STATE{stage}_BYTES = {bytes_aligned}")

print("\n" + "=" * 60)
print("STATE_OFFSET values (offset within 32-byte record):")
for stage in range(K + 1):
    bits_remaining = N - stage * (N // K)
    bytes_exact = int((bits_remaining + 7) // 8)
    offset = 32 - bytes_exact
    print(f"STATE{stage}_OFFSET = {offset}")
