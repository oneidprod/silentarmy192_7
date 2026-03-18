# Change this path if the SDK was installed in a non-standard location
# OPENCL_HEADERS = "/opt/AMDAPPSDK-3.0/include"
OPENCL_HEADERS = "/usr/lib/x86_64-linux-gnu/beignet/include/"
# By default libOpenCL.so is searched in default system locations, this path
# lets you adds one more directory to the search path.
# LIBOPENCL = "/opt/amdgpu-pro/lib/x86_64-linux-gnu"
LIBOPENCL = "/usr/lib/x86_64-linux-gnu/beignet/"

CC = gcc
CPPFLAGS = -I${OPENCL_HEADERS}
CFLAGS = -O3 -std=gnu99 -pedantic -Wextra -Wall \
    -Wno-deprecated-declarations \
    -Wno-overlength-strings
LDFLAGS = -rdynamic -L${LIBOPENCL}
# LDLIBS = -lOpenCL -lrt
LDLIBS= -lOpenCL -lrt
OBJ = main.o blake.o sha256.o
INCLUDES = blake.h param.h _kernel.h sha256.h

all : sa-solver

sa-solver : ${OBJ}
	${CC} -o sa-solver ${OBJ} ${LDFLAGS} ${LDLIBS}

${OBJ} : ${INCLUDES}

_kernel.h : input.cl param.h
	echo 'const char *ocl_code = R"_mrb_(' >$@
	cpp $< >>$@
	echo ')_mrb_";' >>$@

test : sa-solver
	@echo Testing...
	@if res=`./sa-solver --nonces 100 -v -v 2>&1 | grep Soln: | \
	    diff -u testing/sols-100 -`; then \
	    echo "Test: success"; \
	else \
	    echo "$$res\nTest: FAILED" | cut -c 1-75 >&2; \
	fi
#	When compiling with NR_ROWS_LOG != 20, the solutions it finds are
#	different: testing/sols-100

clean :
	rm -f sa-solver sa-tromp _kernel.h *.o _temp_*

re : clean all

# CPU Tromp baseline solver (Phase 0)
cpu_tromp_baseline : cpu_tromp_baseline.o blake.o sha256.o
	${CC} -o cpu_tromp_baseline cpu_tromp_baseline.o blake.o sha256.o ${LDFLAGS}

cpu_tromp_baseline.o : cpu_tromp_baseline.c blake.h param.h sha256.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c cpu_tromp_baseline.c

# Test verifier — uses Tromp's blake2b to verify eq1927 reference solutions
test_verifier : test_verifier.o blake/blake2b.o
	${CC} -o test_verifier test_verifier.o blake/blake2b.o ${LDFLAGS}

test_verifier.o : test_verifier.c param.h blake/blake2.h
	${CC} ${CPPFLAGS} ${CFLAGS} -Iblake -c test_verifier.c

# Blake2b comparison tool (existing)
compare_blake2b : compare_blake2b.o blake.o
	${CC} -o compare_blake2b compare_blake2b.o blake.o ${LDFLAGS} ${LDLIBS}

compare_blake2b.o : compare_blake2b.c blake.h param.h _kernel.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c compare_blake2b.c

# sa-tromp: Standalone Equihash 192,7 GPU miner with Stratum pool support
sa-tromp : sa-tromp.o compress_sol.o stratum.o blake.o blake/blake2b.o
	${CC} -o sa-tromp sa-tromp.o compress_sol.o stratum.o blake.o blake/blake2b.o \
	    ${LDFLAGS} ${LDLIBS} -lpthread

sa-tromp.o : sa-tromp.c blake.h param.h _kernel.h solution_extraction.c \
             blake/blake2.h compress_sol.h stratum.h
	${CC} ${CPPFLAGS} ${CFLAGS} -Iblake -c sa-tromp.c

compress_sol.o : compress_sol.c compress_sol.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c compress_sol.c

stratum.o : stratum.c stratum.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c stratum.c

# Compression unit test (no OpenCL needed)
test_compress_sol : test_compress_sol.o compress_sol.o
	${CC} -o test_compress_sol test_compress_sol.o compress_sol.o

test_compress_sol.o : test_compress_sol.c compress_sol.h
	${CC} ${CFLAGS} -c test_compress_sol.c

blake/blake2b.o : blake/blake2b.cpp blake/blake2.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c blake/blake2b.cpp -o blake/blake2b.o
