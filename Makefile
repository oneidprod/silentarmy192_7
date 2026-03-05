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
	rm -f sa-solver _kernel.h *.o _temp_*

re : clean all

# CPU Tromp baseline solver (Phase 0)
cpu_tromp_baseline : cpu_tromp_baseline.o blake.o sha256.o
	${CC} -o cpu_tromp_baseline cpu_tromp_baseline.o blake.o sha256.o ${LDFLAGS}

cpu_tromp_baseline.o : cpu_tromp_baseline.c blake.h param.h sha256.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c cpu_tromp_baseline.c

# Test verifier (existing tool)
test_verifier : test_verifier.o blake.o sha256.o
	${CC} -o test_verifier test_verifier.o blake.o sha256.o ${LDFLAGS}

test_verifier.o : test_verifier.c blake.h param.h sha256.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c test_verifier.c

# Blake2b comparison tool (existing)
compare_blake2b : compare_blake2b.o blake.o
	${CC} -o compare_blake2b compare_blake2b.o blake.o ${LDFLAGS} ${LDLIBS}

compare_blake2b.o : compare_blake2b.c blake.h param.h _kernel.h
	${CC} ${CPPFLAGS} ${CFLAGS} -c compare_blake2b.c
