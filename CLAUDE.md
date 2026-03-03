# Claude's Action Plan - Equihash 192,7 Solution Strategy

## Mission Objective
**Get pools to accept the silentarmy192_7 miner's valid solutions**

## Anti-Death Spiral Protocol

### Success Criteria (Clear Exit Conditions)
- ✅ **Primary**: Pool accepts submitted solutions (benchmark finds solutions but pool rejects)
- ✅ **Secondary**: Understand why pool rejects valid benchmark solutions
- ❌ **Failure**: Still pool rejection after completing all planned phases

### Current Status Verified
- ✅ **Benchmark works**: Solver finds multiple solutions per nonce (confirmed)
- ❌ **Pool rejection**: Valid solutions are rejected by pools
- 🎯 **Focus**: Solution format/protocol compatibility with pools

### Time-Boxing Strategy
- **Maximum 3 investigation phases** (detailed below)
- **Maximum 2 hours per phase** before mandatory commit + assessment
- **Maximum 6 hours total** before documenting pool compatibility issues

## Phase-by-Phase Battle Plan

### Phase 1: Solution Format Analysis (Current Priority)
**Time Limit**: 45 minutes  
**Objective**: Compare benchmark solution format vs pool expectations

**Tasks**:
1. Analyze current solution encoding in benchmark output
2. Compare solution byte format vs pool protocol specifications
3. Check endianness, compact size encoding, header format
4. Test solution submission to different pool endpoints
5. **Commit Point**: `"pool: identify solution format differences"`

**Success**: Find specific encoding/format mismatch causing rejections  
**Failure Trigger**: Solutions appear correctly formatted  
**Fallback**: Skip to Phase 2 (protocol analysis)

### Phase 2: Pool Protocol Analysis
**Time Limit**: 60 minutes  
**Objective**: Verify nonce handling and job submission protocol

**Tasks**:
1. Check job ID handling and work unit format
2. Verify nonce incrementation vs pool expectations
3. Test different pool software (stratum, getwork, etc.)
4. Analyze network communication logs
5. **Commit Point**: `"pool: protocol compatibility analysis"`

**Success**: Identify protocol/communication issue  
**Failure Trigger**: Protocol appears correct  
**Fallback**: Skip to Phase 3 (pool compatibility testing)

### Phase 3: Pool Compatibility Testing
**Time Limit**: 60 minutes  
**Objective**: Test with different pools and identify working combinations

**Tasks**:
1. Test submission to different Zero cryptocurrency pools
2. Try various pool endpoints and protocols
3. Compare against other working Equihash 192,7 miners
4. Document successful vs failed pool combinations
5. **Commit Point**: `"pool: compatibility test results"`

**Success**: Find pools that accept solutions or identify miner-specific issues  
**Failure Trigger**: All pools reject apparently valid solutions  
**Fallback**: Document pool incompatibility

### Phase 4: OpenCL Reset Option ("Server Restart") - REFERENCE ONLY
**Note**: This is not a debugging phase, just a reminder for fixing OpenCL corruption

**When OpenCL/GPU corruption occurs (symptoms: clCreateBuffer errors, zero solutions in benchmark)**:
1. Clean OpenCL state: `make clean && rm -f _kernel.h`
2. Reset GPU driver: `sudo rmmod nvidia && sudo modprobe nvidia` (if NVIDIA)
3. Fresh build: `make -j4` with clean kernel compilation
4. Test: Single nonce run to verify OpenCL pipeline works

**Note**: No git commit needed - this is just environment cleanup

## Git Commit Strategy

### Commit Frequency
- **Every 30-45 minutes** or at logical breakpoints
- **Before any major changes** (solution format changes, protocol modifications)
- **After each phase completion** (success or failure)

### Commit Message Format
```
<category>: <brief description>

- <specific change 1>
- <specific change 2>
- Status: <working|broken|partial>
- Next: <next planned step>
```

### Branch Management
- **Current branch**: `pool-compatibility-fixes`
- **Backup branch**: `pre-pool-debugging-backup` (create before Phase 1)
- **Test branch**: `pool-format-testing` (for Phase 2-3 if needed)

## Decision Trees

### When to Move to Next Phase
```
IF (current_phase_objective_achieved) 
   → Continue to next phase
ELSE IF (time_limit_exceeded)
   → Commit current state + move to next phase  
ELSE IF (clearly_not_working)
   → Jump to fallback phase
```

### When to Use OpenCL Reset
```
IF (benchmark_shows_zero_solutions) OR (clCreateBuffer_errors)
   → Use OpenCL reset (reference section below)
IF (driver_instability) OR (kernel_build_broken)
   → OpenCL reset
NOTE: Current status shows benchmark working - OpenCL reset not needed
```

## Fallback Strategies

### Immediate Circuit Breakers
1. **Build Corruption**: `make clean && rm -f _kernel.h && make`
2. **OpenCL Errors**: Use OpenCL reset (see reference section)
3. **Segfault Return**: Revert to last stable commit
4. **Time Exceeded**: Force commit + assessment

### "Server Restart" Protocol Detail
The "restart the server" approach = **reset OpenCL/GPU state to fix corruption**:

1. **Preserve work**: `git add -A && git commit -m "checkpoint before restart"`
2. **Clean OpenCL**: `make clean && rm -f _kernel.h`
3. **Reset GPU driver**: Check `dmesg` for GPU errors, restart driver if needed
4. **Fresh rebuild**: `make -j4` to regenerate kernel from source
5. **Test minimal**: Single nonce run to verify OpenCL pipeline works

## Rate Limiting Strategy

### Tool Usage Optimization
- **Batch file operations**: Use `multi_replace_string_in_file` instead of sequential edits
- **Combine read operations**: Read larger file sections rather than many small chunks
- **Cache grep results**: Store search results in variables, don't re-search
- **Consolidate runs**: Combine `make && run && parse` into single command chains

### Pacing Guidelines
- **Wait 2-3 seconds** between major tool invocations
- **Group related operations** (read → edit → build → test) into focused bursts
- **Use terminal efficiently**: Chain commands with `&&` and `||` operators
- **Document findings** in variables/files rather than re-querying

## Success Metrics

### Phase 1 Success
- Identify solution format/encoding issue causing pool rejection
- Understand differences between benchmark vs pool submission format

### Phase 2 Success  
- Find protocol/communication issue in pool interaction
- Fix nonce handling or job submission process

### Phase 3 Success
- Find compatible pools or document specific compatibility requirements

## Emergency Procedures

### If Completely Stuck
1. **Document current state**: Update DEBUGGING_SESSION_SUMMARY.md
2. **Create issue report**: List what was tried and results
3. **Suggest alternative approaches**: Different pool, solution format changes, etc.
4. **Clean exit**: Leave codebase in buildable state

### If Infinite Loop Detected
1. **Immediate stop**: Don't continue same approach
2. **Time check**: If >2 hours elapsed, move to next phase
3. **Sanity check**: Are we making measurable progress toward pool acceptance?

## Current Status Checkpoint

**Starting Phase**: 1 (Solution Format Analysis)  
**Confirmed Status**: Benchmark finds multiple solutions per nonce  
**Current Issue**: Pool rejection of valid solutions  
**Time Started**: 2026-03-03  
**Last Commit**: d669303  
**Backup Created**: pre-pool-debugging-backup

---

**Remember**: The goal is pool acceptance, not perfect debugging. If we're not getting closer to pool acceptance after each phase, we pivot or document incompatibility. No endless rabbit holes.