ROOT_PATH=.

# shared toolchain definitions
INC = -I$(ROOT_PATH)/inc
CFLAGS  = -g -Wall -D_GNU_SOURCE $(INC) -lstdc++ -O3
EXTRA_CFLAGS = -lm
LDFLAGS_SHARED = 
LDFLAGS_STATIC = 
LD      = gcc
CC      = gcc
LDXX	= g++
CXX	= g++
AR      = ar

ifeq ($(DEBUG), y)
	CFLAGS += -D__DEBUG__
endif

ifeq ($(TIMERS), y)
	CFLAGS += -D__TIMERS__
endif

MLX5_INC = -I$(ROOT_PATH)/rdma-core/build/include
MLX5_LIBS = -L$(ROOT_PATH)/rdma-core/build/lib/statics -L$(ROOT_PATH)/rdma-core/build/util -L$(ROOT_PATH)/rdma-core/build/ccan
MLX5_LIBS +=  -lmlx5 -libverbs -lrdma_util -lccan -lnl-3 -lnl-route-3  -lpthread -ldl -lnuma -lpthread

ifeq ($(CONFIG_MLX5),y)
CFLAGS += -DMLX5
LDFLAGS_SHARED += $(MLX5_LIBS)
LDFLAGS_STATIC += $(MLX5_LIBS)
INC += $(MLX5_INC)
endif

#binary name
APP = mlx5-netperf
# libbase.a - the base library
base_src = $(wildcard base/*.c)
base_obj = $(base_src:.c=.o)

# main - the main binary
SRCS-y := main.c mlx5_init.c mlx5_rxtx.c latency.c requests.c time.c mempool.c mem.c pci.c bitmap.c sysfs.c 

all: shared
.PHONY: shared static
shared: build/$(APP)-shared
	ln -sf $(APP)-shared build/$(APP)
static: export build/$(APP)-static
	ln -sf $(APP)-static build/$(APP)

build/$(APP)-shared: $(SRCS-y) Makefile | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(EXTRA_CFLAGS) $(LDFLAGS) $(LDFLAGS_SHARED)

build/$(APP)-static: $(SRCS-y) Makefile | build
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(EXTRA_CFLAGS) $(LDFLAGS) $(LDFLAGS_STATIC)

build:
	@mkdir -p $@


.PHONY: submodules
submodules:
	$(ROOT_PATH)/init_submodules.sh

.PHONY: clean
clean:
	rm -f build/$(APP) build/$(APP)-static build/$(APP)-shared
	test -d build && rmdir -p build || true



