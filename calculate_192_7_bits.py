#!/usr/bin/env python3
# Generate bit allocation and byte offset table for Equihash 192,7

print("=== Equihash 192,7 Bit Allocation ===\n")

# For 192,7 with 256 buckets
N = 192
K = 7
BUCKET_BITS = 8
COLLISION_BITS = 8  # For simplicity with 256 buckets

bits_remaining = N
for stage in range(K + 1):
    if stage < K:
        # Bucket bits are the highest 8 bits
        bucket_high = bits_remaining - 1
        bucket_low = bits_remaining - BUCKET_BITS
        
        # Collision bits are the next 8 bits
        collision_high = bucket_low - 1
        collision_low = bucket_low - COLLISION_BITS
        
        print(f"[{bucket_high}:{bucket_low}] Selects bucket in State {stage}")
        print(f"  [{collision_high}:{collision_low}] Collision search in Stage {stage+1}")
        
        # Calculate byte offset and shift for reading collision bits
        # Collision bits start at bit collision_high
        byte_offset = collision_high // 8
        bit_in_byte = collision_high % 8
        
        # Need to read collision_high down to collision_low (8 bits total)
        # If bits are aligned to byte boundary, no shift needed
        # Otherwise need to shift
        
        bytes_in_xorwork = (bits_remaining + 7) // 8
        bytes_aligned = ((bytes_in_xorwork + 3) // 4) * 4
        
        # Following xenoncat's pattern:
        # Read from byte offset, shift to align the collision bits to low byte
        if (collision_low % 8) == 0:
            # Collision bits end at byte boundary
            method = f"[{byte_offset}]"
        else:
            shift_amount = collision_low % 8
            method = f"[{byte_offset}]>>{shift_amount}"
        
        print(f"  Method: {method}")
        print(f"  Bytes in Xorwork: {bytes_in_xorwork}, aligned: {bytes_aligned}")
        print()
        
        # Move to next stage
        bits_remaining -= (BUCKET_BITS + COLLISION_BITS)
    else:
        print(f"State {stage}: {bits_remaining} bits remaining")

print("\n=== STATE_BYTES for 192,7 ===")
bits_remaining = N
for stage in range(K + 1):
    bytes_exact = (bits_remaining + 7) // 8
    bytes_aligned = ((bytes_exact + 3) // 4) * 4
    print(f"STATE{stage}_BYTES = {bytes_aligned}")
    
    if stage < K:
        bits_remaining -= (BUCKET_BITS + COLLISION_BITS)

print("\n=== Byte offset method for collision extraction ===")
bits_remaining = N
for stage in range(K):
    bucket_high = bits_remaining - 1
    collision_high = bits_remaining - BUCKET_BITS - 1
    collision_low = collision_high - COLLISION_BITS + 1
    
    # The collision bits span from collision_high to collision_low
    # We need to read these 8 bits
    byte_containing_high = collision_high // 8
    
    # For 192,7, collision bits should be at specific byte offsets
    # After XORing, we read from [base+offset] where base points to start of STATE data
    
    bytes_in_state = (bits_remaining + 7) // 8
    
    print(f"Stage {stage}: collision bits [{collision_high}:{collision_low}]")
    print(f"  Byte {byte_containing_high} contains bit {collision_high}")
    print(f"  State has {bytes_in_state} bytes")
    print(f"  Read from byte offset: {byte_containing_high}")
    
    bits_remaining -= (BUCKET_BITS + COLLISION_BITS)
