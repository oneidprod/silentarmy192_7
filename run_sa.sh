#!/bin/sh

# Set LD_LIBRARY_PATH
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu/beignet":$LD_LIBRARY_PATH

./sa-solver -n 192 -k 7 -v
#./sa-solver -n 200 -k 9
#./sa-solver -h