SNDFILE_INCS=`pkg-config --cflags sndfile`
SNDFILE_LIBS=`pkg-config --libs sndfile`

HDCD_INCS=`pkg-config --cflags libhdcd`
HDCD_LIBS=`pkg-config --libs libhdcd`

all: hdcd-fixup
clean:
	rm -f *.o hdcd-fixup

hdcd-fixup.o: hdcd-fixup.c
	$(CC) -Wall -O2 -c $(SNDFILE_INCS) $(HDCD_INCS) -D_XOPEN_SOURCE=500 hdcd-fixup.c

hdcd-fixup: hdcd-fixup.o
	$(CC) -o hdcd-fixup hdcd-fixup.o $(SNDFILE_LIBS) $(HDCD_LIBS)