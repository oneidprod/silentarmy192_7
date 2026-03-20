export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu/beignet":$LD_LIBRARY_PATH

# pooly.ca (confirmed working — ACCEPTED shares)
./sa-tromp -p 0 \
  -o stratum+tcp://pooly.ca:3050 \
  -u t1TCgwxZ3RMpWtg3Tu5qk8BcNdawRHeJd1g.test \
  -P x

# zpool (public pool — higher difficulty, needs ~1hr for accepted share)
#./sa-tromp -p 0 \
#  -o stratum+tcp://equihash192.na.mine.zpool.ca:2192 \
#  -u t1TCgwxZ3RMpWtg3Tu5qk8BcNdawRHeJd1g \
#  -P "c=ZER,zap=ZER"

# local test snomp server
#./sa-tromp -p 0 \
#  -o stratum+tcp://192.9.246.79:3092 \
#  -u t1e6nAkZLoXUgwRuJ9qj2CF15qkroWVsVVQ.test \
#  -P x
