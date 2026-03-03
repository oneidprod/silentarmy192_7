# Claude Haiku 4.5 - Pool Compatibility Debugging Plan

## Mission Objective
**Get pools to accept silentarmy192_7 valid solutions - diagnose and fix "invalid solution" error 20/21**

---

## Anti-Death Spiral Protocol

### Success Criteria (Clear Exit Conditions)
- ✅ **Primary**: Pool accepts at least ONE solution (benchmark finds solutions, pool should accept them)
- ✅ **Secondary**: Identify root cause of error 20/21 rejection from pool
- ❌ **Failure**: Still pool rejection after completing all phases

### Current Status (VERIFIED)
- ✅ **Benchmark works**: Solver finds multiple solutions per nonce consistently
- ✅ **Blake2b correct**: Zero uses Blake2b with "ZERO_PoW" personalization (verified in Zero source)
- ✅ **Parameters correct**: Using 192,7 matching Zero mainnet/testnet
- ✅ **Error 25 fixed**: Race condition resolved via subscription checks
- ❌ **Error 20/21**: Pool rejects solutions as "invalid solution" - THIS IS THE BLOCKER

### Critical Discoveries from Zero Source Analysis
1. **Block Input Structure**: CEquihashInput serializes only header fields (NOT nonce/solution)
   - Includes: nVersion, hashPrevBlock, hashMerkleRoot, hashFinalSaplingRoot, nTime, nBits
   - Then appended: nNonce (32 bytes)
   - NOT serialized: nSolution
   
2. **Equihash params**: Confirmed 192,7 for mainnet/testnet
   - Personalization: "ZERO_PoW" + [192,0,0,0] + [7,0,0,0] in little-endian
   - Hash output: 48 bytes (not 50 - we have this correct)

3. **Current Code**: Already using Blake2b correctly - NOT the issue

### Root Cause Hypothesis
- NOT algorithmic (Blake2b, personalization, N/K are all correct)
- Likely: Solution format/encoding mismatch between miner and pool verification

---

## Time-Boxing Strategy (DEATH SPIRAL PREVENTION)

### Maximum Limits
- **Total allocated time**: 4 hours maximum
- **Per-phase timeout**: 45-60 minutes each
- **Mandatory checkpoint**: After each phase, assess progress/pivot

### Phase Structure
- **Phase 0**: Validation (10 min) - verify local build works
- **Phase 1**: Pool verification analysis (30 min) - understand what pool expects
- **Phase 2**: Solution format analysis (45 min) - compare byte-level encoding
- **Phase 3**: Targeted testing (30 min) - if Phase 2 finds specific mismatch

### Circuit Breaker Rules
- If no progress after 45 minutes: **Move to next phase** (don't spin on same problem)
- If Phase 2 finds no byte-level difference: **Escalate to user** (ask for Sonnet 4)
- If all phases complete with no solution: **Document findings and exit**

---

## Phase-by-Phase Execution Plan

### Phase 0: Local Validation (10 min)
**Time Limit**: 10 minutes  
**Objective**: Verify rebuild doesn't break anything, confirm local solving still works

**Actions**:
1. `make clean && make` - fresh build
2. Verify no build errors
3. `./run_sa.sh` - local test for 30 seconds
4. Check for any solutions in output

**Success Signal**:
- Build succeeds
- Local solver produces solutions (>0 sol/s)
- No crashes

**Failure Signal**:
- Build fails → revert to pre-pool-debugging-backup branch
- Zero solutions → OpenCL reset (see CLAUDE.md reference)

**Next Step if Success**: Proceed to Phase 1

---

### Phase 1: Pool Verification Analysis (30 min)
**Time Limit**: 30 minutes  
**Objective**: Understand pool's verification expectations and constraints

**Actions**:
1. Locate equihashverify npm module in stratum-lib or pool-source
2. Find verification algorithm:
   - What parameters does it use (N, K, personalization)?
   - What data does it expect hashed?
   - What output format for solution?
3. Check algoProperties.js for Zero coin configuration
4. Identify where error 20/21 is generated in pool code
5. Compare pool's expected solution format to our tx output

**Evidence to Collect**:
- Exact pool verification function signature
- Parameters being passed to verify (N, K, personalization, solution format)
- Solution input format (hex string vs raw bytes, prefix or not)

**Success Signal**:
- Identified solution format pool expects
- Know if pool uses correct N, K, personalization

**Failure Signal**:
- Cannot find verification code (unlikely)
- Pool config is empty/wrong

**Next Step if Success**: Proceed to Phase 2

---

### Phase 2: Solution Format Analysis (45 min)
**Time Limit**: 45 minutes  
**Objective**: Identify byte-level encoding differences between our output and pool expectations

**Actions**:
1. Generate one valid solution locally
2. Extract raw solution bytes from our miner output
3. Analyze structure:
   - Is fd9001 prefix included or separate?
   - Solution is 400 raw bytes (800 hex chars)
   - Are indices packed as 25-bit values in big-endian?
   - Correct bit ordering?
4. If possible, compare against nheqminer output (reference implementation)
5. Document exact byte layout we're sending vs what pool expects
6. Test with variations:
   - Try sending solution WITHOUT fd9001 prefix
   - Try reversed endianness if indicated
   - Check if solution needs different format

**Evidence to Collect**:
- Hex dump of our solution (first 40 chars, last 40 chars)
- Hex dump of expected format from pool code
- Byte-for-byte comparison

**Success Signal**:
- Found specific byte-level difference (encoding, endianness, prefix handling)
- Know exact change needed

**Failure Signal**:
- No byte-level differences found
- Our format matches what pool expects but still rejects

**Pivot if Failure**: Skip Phase 3, escalate to user (ask for Sonnet 4 analysis)

**Next Step if Success**: Proceed to Phase 3 or implement fix

---

### Phase 3: Targeted Submission Testing (30 min)
**Time Limit**: 30 minutes  
**Objective**: Test specific format variations to find what pool accepts

**Actions**:
1. Create several solution submission variations:
   - Version A: Current format (fd9001 + solution)
   - Version B: Without fd9001 prefix
   - Version C: Different endianness if applicable
   - Version D: Different personalization (if not validated locally)
2. Submit each to pool, document response
3. Look for ANY variation that gets different error (indicates format recognition)

**Success Signal**:
- Find variation that accepted or gets different error (e.g., error 1 instead of error 20)
- This signals format is correct for that variation

**Failure Signal**:
- All variations still get error 20

**Next Step if Success**: Commit fix and update code

---

## Git Commit Strategy

### Commit Frequency
- **Before each phase**: If significant code changes planned
- **After each phase**: Regardless of outcome (document findings)
- **On any success**: Immediately with clear message

### Commit Message Format
```
<category>: <brief description>

- <specific finding 1>
- <specific finding 2>
- Analysis: <what was tried>
- Result: <success/failure>
- Next: <planned step or reason for pivot>
```

### Branch Strategy
- **Current branch**: `solution-fix`
- **Checkpoint commits**: Tagged with phase (e.g., "phase0-validation", "phase1-pool-analysis")
- **Rollback point**: `pre-pool-debugging-backup` (exists - do NOT modify)

### Mandatory Commits
1. **After Phase 0**: `diag: validate local build - solutions producing correctly`
2. **After Phase 1**: `diag: pool verification analysis - document expectations`
3. **After Phase 2**: `diag: solution format analysis - identify byte-level differences`
4. **On success**: `fix: pool compatibility - <specific change made>`

---

## Rate Limiting & Tool Efficiency Strategy

### Tool Usage Optimization
- **Batch grep searches**: Use multiple queries in one grep_search call where possible
- **Parallel file reads**: Read multiple related files in parallel with read_file
- **Combine related operations**: Link file exploration, analysis, and summary into logical bursts
- **Avoid redundant searches**: Cache results in working notes, don't re-search same terms

### Pacing for Reliability
- **2-3 second delay** between major tool invocations
- **Group related operations** (search → read → analyze) into focused bursts
- **Chain terminal commands** with `&&` to reduce command overhead
- **Document findings** in analysis before proceeding to next phase

### Rate Limit Awareness
- **Current**: Using Claude Haiku 4.5 (generous limits)
- **Budget**: ~200k tokens available
- **Phase 0-1**: Should use <20% budget
- **Phase 2**: Concentrated analysis phase, may use 30-40% budget
- **Escalation ready**: If stuck, will request handoff to Sonnet 4 with full context

---

## Decision Trees

### Should I continue or escalate?

```
IF (Phase 0 validation fails)
   → Revert to pre-pool-debugging-backup + rebuild
   THEN → Retry Phase 0
   IF (still fails) → ESCALATE to user

IF (Phase 1 complete AND no mysteries found)
   → Pool config/code is clear
   → Proceed to Phase 2

IF (Phase 2 complete AND no byte differences found)
   → Solution format appears correct
   → ESCALATE to Sonnet 4 (may be subtle issue)
   
IF (Phase 2 finds specific difference)
   → Create targeted fix
   → Commit with evidence
   → Test pool submission
```

---

## Success Metrics

### Phase 0 Success
✓ Build completes without errors
✓ Local solver produces >0 solutions/sec
✓ No crashes or segfaults

### Phase 1 Success
✓ Identified pool verification function
✓ Know exact parameters pool uses (N, K, personalization)
✓ Know solution format pool expects
✓ Understand error 20/21 generation

### Phase 2 Success
✓ Have byte-level analysis of solution encoding
✓ Identified specific format difference (if exists)
✓ Know if issue is endianness, packing, prefix, or structure

### Phase 3 Success (if reached)
✓ Found submission format that works
✓ Pool accepted at least one solution
✓ Error changed from 20 to acceptance or different error

---

## Fallback Procedures

### If Build Breaks
```bash
git checkout pre-pool-debugging-backup
make clean && make
./run_sa.sh
```

### If Zero Solutions Found
```bash
# OpenCL/GPU issue - reset state
make clean && rm -f _kernel.h
make -j4
./run_sa.sh
```

### If Time Limit Reached
- Document current findings in commit
- Escalate to user with specific recommendations
- Leave codebase in compilable state

### If Completely Stuck
1. **Document findings**: Create POOL_DEBUG_FINDINGS.md with:
   - What was verified to work (local solving, Blake2b, params)
   - What doesn't work (pool rejection)
   - What was tested (list of attempted fixes)
2. **Commit analysis**: `diag: pool debugging inconclusive - escalate`
3. **Request escalation**: Ask for Sonnet 4 or user input

---

## Execution Checklist

Before starting each phase:
- [ ] Previous phase completed or explicitly skipped
- [ ] Git status clean or changes committed
- [ ] Understand phase objectives
- [ ] Know success/failure signals
- [ ] Timer set for phase timeout
- [ ] Evidence collection documented

After each phase:
- [ ] Results documented
- [ ] Findings committed to git
- [ ] Proceed to next phase or pivot
- [ ] No ambiguity about next step

---

## Current Starting State

**What works**:
- ✅ Local solver produces solutions (run_sa.sh successful)
- ✅ Blake2b verification implemented correctly
- ✅ Equihash 192,7 parameters correct
- ✅ Error 25 race condition fixed
- ✅ Nonce size correct (32 bytes)
- ✅ fd9001 prefix handling fixed (single prefix)

**What doesn't work**:
- ❌ Pool accepts solutions (error 20/21 "invalid solution")

**Starting action**: **BEGIN PHASE 0**

---

## Notes for Reviewer

- This plan follows CLAUDE.md anti-death spiral protocol
- Time-boxed to prevent endless rabbit holes (max 4 hours total)
- Tool usage optimized for rate limiting
- Clear exit conditions at each phase
- Escalation path ready if stuck
- Git history will document all findings
- No code changes until Phase 2 analysis complete

---

**Last Updated**: 2026-03-03  
**Model**: Claude Haiku 4.5  
**Status**: Ready to execute Phase 0
