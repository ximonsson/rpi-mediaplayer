CC      = gcc
AR      = ar
SRCDIR  = src
BUILD   = build
BIN     = bin
SRC     = player.c packet_buffer.c helpers.c
OBJ     = $(addprefix $(BUILD)/, $(SRC:.c=.o))
EXEC    = $(BIN)/player
LIB     = lib/librpi_mp.a
VC      = /opt/vc

ifdef VERBOSE
CFLAGS += -v
endif

# if we are cross compiling
ifdef CROSS
CC       = arm-linux-gnueabihf-gcc
AR       = arm-linux-gnueabihf-ar
SYSROOT  = $(HOME)/mnt/penguin
VC       = $(SYSROOT)/opt/vc
CFLAGS  += -mfloat-abi=hard -march=armv6 -mfpu=vfp -marm --sysroot=$(SYSROOT)
endif

DEFINES = -DSTANDALONE \
          -D__STDC_CONSTANT_MACROS \
          -D__STDC_LIMIT_MACROS \
          -DTARGET_POSIX \
          -D_LINUX \
          -DPIC \
          -D_REENTRANT \
          -D_LARGEFILE64_SOURCE \
          -D_FILE_OFFSET_BITS=64 \
          -U_FORTIFY_SOURCE  \
          -DHAVE_LIBOPENMAX=2 \
          -DOMX \
          -DOMX_SKIP64BIT \
          -DUSE_EXTERNAL_OMX \
          -DHAVE_LIBBCM_HOST \
          -DUSE_EXTERNAL_LIBBCM_HOST \
          -DUSE_VCHIQ_ARM

CFLAGS += -Wall \
          -O3 \
          -fPIC \
          -ftree-vectorize \
          -pipe \
          -Wno-psabi
          #-g3

INCLUDES = -I./include \
           -I$(VC)/include \
           -I$(VC)/include/interface/vcos/pthreads \
           -I$(VC)/include/interface/vmcs_host/linux \
           -I$(VC)/src/hello_pi/libs/ilclient \
           -I$(VC)/src/hello_pi/libs/vgfont

LDPATH += -L./lib \
          -L$(VC)/src/hello_pi/libs/ilclient \
          -L$(VC)/lib \
          -L$(VC)/src/hello_pi/libs/vgfont

LIBS = -lrpi_mp \
       -lilclient \
       -lopenmaxil \
       -lbcm_host \
       -lGLESv2 \
       -lEGL \
       -lvcos \
       -lvchiq_arm \
       -lpthread \
       -lrt \
       -lavcodec \
       -lavutil \
       -lavformat \
       -lm

ARARGS = rcs


all: lib bin

lib: $(LIB)

bin: $(EXEC)

$(LIB): $(OBJ)
	@mkdir -p $(@D)
	$(AR) $(ARARGS) $@ $^

$(EXEC): lib
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) $(LDPATH) -o $@ main.c $(LIBS)

$(BUILD)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(DEFINES) $(CFLAGS) $(INCLUDES) -c -o $@ $<

clean:
	rm -rf $(BUILD)/*.o $(BIN)/* $(LIB)
