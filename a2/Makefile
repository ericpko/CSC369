all: carsim

CFILES=$(wildcard *.c)
ObjFILES=$(CFILES:.c=.o)
HFILES=$(wildcard *.h)

carsim: $(ObjFILES)
	$(CC) $(CFLAGS) -Wall -pthread -o $@ $^

%.o: %.c $(HFILES)
	gcc $(CFLAGS) -Wall -pthread -o $@ -c $<

clean:
	rm -f *.o carsim
