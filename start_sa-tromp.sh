export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu/beignet":$LD_LIBRARY_PATH
# zpool
#./sa-tromp -p 0 \
#  -o stratum+tcp://equihash192.na.mine.zpool.ca:2192 \
#  -u t1TCgwxZ3RMpWtg3Tu5qk8BcNdawRHeJd1g \
#  -P "test,c=ZER,zap=ZER"

# sa-tromp -p 0 \
# -o stratum+tcp://192.9.246.79:3092 \
# -u t1e6nAkZLoXUgwRuJ9qj2CF15qkroWVsVVQ.test \
# -P x

./sa-tromp -p 0 -o stratum+tcp://127.0.0.1:9999 -u t1e6nAkZLoXUgwRuJ9qj2CF15qkroWVsVVQ.test -P x 2>&1 | tee /tmp/miner_output.txt

