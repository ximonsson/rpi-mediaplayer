CC		= gcc
SRCDIR	= src
BUILD	= build
SRC		= main.c video.c packet_buffer.c helpers.c
OBJ		= $(addprefix $(BUILD)/, $(SRC:.c=.o))
EXEC	= bin/mediaplayer

FFMPEG	= /home/pi/ffmpeg_compiled/usr/local

CFLAGS += -DSTANDALONE \
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
		  -Wno-psabi \
		  -I./include
		  #-g3

CFLAGS += -I/opt/vc/include \
		  -I/opt/vc/include/interface/vcos/pthreads \
		  -I/opt/vc/include/interface/vmcs_host/linux \
		  -I/opt/vc/src/hello_pi/libs/ilclient \
		  -I/opt/vc/src/hello_pi/libs/vgfont \

LIBS+=-L/opt/vc/lib/ \
	  -L/opt/vc/src/hello_pi/libs/ilclient \
      -L/opt/vc/src/hello_pi/libs/vgfont \
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


all: $(EXEC)

$(EXEC): $(OBJ)
	@mkdir -p $(@D)
	$(CC) -o $@ $^ $(LIBS)

$(BUILD)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)/*.o $(EXEC)
