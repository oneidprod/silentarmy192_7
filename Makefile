OPENCL_HEADERS = "/usr/include"
LIBOPENCL      = "/lib/x86_64-linux-gnu"

CC      = gcc
CPPFLAGS = -I${OPENCL_HEADERS} -DCL_TARGET_OPENCL_VERSION=120
CFLAGS  = -O3 -march=native -std=gnu99 -pedantic -Wextra -Wall \
    -Wno-deprecated-declarations \
    -Wno-overlength-strings
LDFLAGS = -L${LIBOPENCL}
LDLIBS  = -lOpenCL -lrt

.PHONY : all clean re

all : sa-tromp

_kernel.h : input.cl param.h
	echo 'const char *ocl_code = R"_mrb_(' >$@
	cpp $< >>$@
	echo ')_mrb_";' >>$@

# sa-tromp: Standalone Equihash 192,7 GPU miner with Stratum pool support
sa-tromp : sa-tromp.o compress_sol.o stratum.o blake.o blake/blake2b.o sha256.o
	${CC} -o sa-tromp sa-tromp.o compress_sol.o stratum.o blake.o blake/blake2b.o sha256.o \
	    ${LDFLAGS} ${LDLIBS} -lpthread

sa-tromp.o : sa-tromp.c blake.h param.h _kernel.h solution_extraction.c \
             blake/blake2.h compress_sol.h stratum.h sha256.h
	${CC} ${CPPFLAGS} ${CFLAGS} -Iblake -c sa-tromp.c

compress_sol.o : compress_sol.c compress_sol.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c compress_sol.c

stratum.o : stratum.c stratum.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c stratum.c

blake.o : blake.c blake.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c blake.c

sha256.o : sha256.c sha256.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c sha256.c

blake/blake2b.o : blake/blake2b.cpp blake/blake2.h
	${CC} ${CPPFLAGS} -O3 -march=native -Wall -Wno-deprecated-declarations \
	    -c blake/blake2b.cpp -o blake/blake2b.o

# Test verifier — uses Tromp's blake2b to verify eq1927 reference solutions
test_verifier : test_verifier.o blake/blake2b.o
	${CC} -o test_verifier test_verifier.o blake/blake2b.o ${LDFLAGS}

test_verifier.o : test_verifier.c param.h blake/blake2.h
	${CC} ${CPPFLAGS} ${CFLAGS} -Iblake -c test_verifier.c

# Compression unit test (no OpenCL needed)
test_compress_sol : test_compress_sol.o compress_sol.o
	${CC} -o test_compress_sol test_compress_sol.o compress_sol.o

test_compress_sol.o : test_compress_sol.c compress_sol.h
	${CC} ${CFLAGS} -c test_compress_sol.c

clean :
	rm -f sa-tromp _kernel.h *.o blake/blake2b.o _temp_*

re : clean all
