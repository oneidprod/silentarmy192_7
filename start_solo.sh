#!/bin/sh

# Set LD_LIBRARY_PATH
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu/beignet":$LD_LIBRARY_PATH

POOL=stratum+tcp://nodejs-pool.home:3093
#POOL=stratum+tcp://158.101.13.27:6969
#POOL=stratum+tcp://equihash192.na.mine.zpool.ca:2192#xnsub
#POOL=stratum+tcp://equihash125.na.mine.zpool.ca:2125
#WALLET=t1JdJNvywTpsnNavx9k97qqegSVwBPkhr9J
#WALLET=t1TCgwxZ3RMpWtg3Tu5qk8BcNdawRHeJd1g
WALLET=t1e6nAkZLoXUgwRuJ9qj2CF15qkroWVsVVQ.test
#ALGO=EQUI192_7
#PASS=5810,c=ZER,zap=ZER,spm=32
#PASS=test,c=FLUX,spm=32
PASS=x
#PERS=ZERO_PoW

#$ silentarmy --use 2,5 -c stratum+tcp://us1-zcash.flypool.org:3333 -u t1cVviFvgJinQ4w3C2m2CfRxg>
./silentarmy -c $POOL -u $WALLET -p $PASS -v -v --debug

