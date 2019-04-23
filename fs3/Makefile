CFLAGS = -Wall -g -std=gnu99

all: readimage

readimage: readimage.o
	gcc ${CFLAGS} -o $@ $^

%.o: %.c readimage.h ex2.h
	gcc ${CFLAGS} -c $<

clean:
	rm -f readimage *.o
