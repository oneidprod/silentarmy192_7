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
LDLIBS= -lcl -lrt
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
