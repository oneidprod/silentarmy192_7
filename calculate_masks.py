#!/usr/bin/env python3
"""
Calculate collision detection masks for Equihash 192,7
"""

def calculate_collision_masks():
    print("Equihash 192,7 collision mask calculation")
    print("=" * 50)
    
    N = 192
    K = 7
    PREFIX = N // (K + 1)  # 192 / 8 = 24 bits
    
    print(f"N = {N}")
    print(f"K = {K}")
    print(f"PREFIX = {PREFIX} bits")
    print()
    
    # For Equihash collision detection:
    # Each round needs to match on PREFIX/K bits
    # For 192,7: 24/7 ≈ 3.43 bits per round
    
    # But collision detection is typically done on byte boundaries
    # For Equihash 192,7, we typically use:
    # - Round 0: match on 3 bits  
    # - Round 1: match on 3 bits
    # etc.
    
    print("Collision bit requirements per round:")
    total_bits = 0
    for round_num in range(K):
        # For 192,7, typically 3-4 bits per round
        if round_num < 4:
            bits_this_round = 4  # First 4 rounds use 4 bits
        else:
            bits_this_round = 3  # Last 3 rounds use 3 bits
        
        total_bits += bits_this_round
        print(f"Round {round_num}: {bits_this_round} bits (total: {total_bits})")
    
    print(f"\nTotal collision bits: {total_bits} (target: {PREFIX})")
    print()
    
    # Calculate masks for first byte collision detection
    print("Collision masks for first byte:")
    for round_num in range(K):
        if round_num % 2 == 0:
            # Even rounds: match on lower 4 bits
            mask = 0x0F
        else:
            # Odd rounds: match on upper 4 bits  
            mask = 0xF0
            
        print(f"Round {round_num}: mask = 0x{mask:02X}")
    
    print()
    print("Alternative nibble-based masks:")
    for round_num in range(K):
        if round_num % 2 == 0:
            # Even rounds: match on lower nibble
            mask = 0x0F
        else:
            # Odd rounds: match on upper nibble
            mask = 0xF0
            
        print(f"Round {round_num}: mask = 0x{mask:02X} (nibble-based)")

if __name__ == "__main__":
    calculate_collision_masks()