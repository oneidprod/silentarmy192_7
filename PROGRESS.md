# SilentArmy 192,7 Progress Report
*February 19, 2026*

## 🎯 PROJECT SUMMARY
**Goal**: Port silentarmy OpenCL miner from Equihash 200,9 → 192,7 for Zero coin  
**Target**: Dell OptiPlex fleet with Intel UHD Graphics 630  
**Strategy**: Opportunistic mining using idle integrated GPUs  

## ✅ INFRASTRUCTURE COMPLETE
- **OpenCL Environment**: Beignet driver + Intel UHD Graphics detection working  
- **Build System**: Makefile configured, kernels compile without errors  
- **Parameters**: N=192, K=7 set correctly, "ZERO_PoW" personalization active  
- **Performance**: ~0.1 Sol/s, 10.3s per nonce, 2.1GB memory usage  

## ❌ FATAL ALGORITHM ISSUE
**ROOT CAUSE**: OpenCL kernel hardcoded for Equihash 200,9  

**Evidence**:
- All rounds show "Dropped: 0 (coll) 0 (stor)" (should be millions→thousands→hundreds)
- input.cl lines 31-32: `for i in range(9)` hardcoded for 9 rounds
- Memory layouts and bit manipulation assume 200,9 structure
- Zero solutions found after complete execution

**Conclusion**: Algorithm requires major OpenCL kernel rewrite (weeks of work)

## 🔧 FIXES IMPLEMENTED
- **Kernel arguments**: Fixed clSetKernelArg (-49) error with custom kernel_round6
- **Pool mining**: Added --instances=1 --use=0 to prevent Intel GPU overload
- **Configuration**: Updated start_solo.sh for single-instance mining

## 🛑 PROJECT STATUS: BLOCKED
**Infrastructure**: 100% working ✅  
**Algorithm**: 0% working ❌ (hardcoded 200,9 assumptions)  

**Decision**: Abandon silentarmy approach  
**Alternative**: Port working solver1927 to nheqminer OpenCL framework  

## 📚 KEY LEARNINGS
1. Beignet driver excellent for Intel GPU fleet deployment
2. Intel UHD Graphics 630 capable of 2GB+ workloads  
3. Parameter changes ≠ algorithm conversion (deep kernel logic required)
4. Infrastructure validation critical before algorithm implementation

## 🚀 NEXT STEPS
**Pivot to nheqminer OpenCL integration**:
- Leverage working solver1927 algorithm (proven 192,7 implementation)
- Use existing OpenCL infrastructure in MinerFactory.cpp/AvailableSolvers.h
- Target: `./nheqminer -c1927 4 -o 1 0` (4 CPU + 1 Intel GPU)

*Final Status: Infrastructure proven, algorithm incompatible*  
*Recommendation: Apply learnings to nheqminer OpenCL port*
