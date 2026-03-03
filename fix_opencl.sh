#!/bin/bash
set -e

echo "=== Fixing OpenCL Environment ==="

# Ensure Intel OpenCL library is executable
# sudo chmod +x /usr/lib/x86_64-linux-gnu/intel-opencl/libigdrcl.so

# Set proper library path
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/beignet

# Verify both platforms are visible
echo "Checking OpenCL platforms..."
clinfo | grep -E "Number of platforms|Platform Name" | head -6

# Clean build
echo "Clean building solver..."
make clean && make

# Test both platforms
echo "Testing platform detection..."
export LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/beignet
./sa-solver --list

# Verify we actually have devices
echo "Checking if ./sa-solver sees devices..."
DEVICE_COUNT=$(./sa-solver --list | grep -c "ID" || echo "0")
echo "Detected $DEVICE_COUNT OpenCL device(s)"

if [ "$DEVICE_COUNT" -ge 1 ]; then
    echo "=== SUCCESS: Devices detected ==="
    echo "Ready for benchmarking with working solution generation"
else
    echo "=== ERROR: No devices detected ==="
    echo "Please check OpenCL installation and configuration."
fi