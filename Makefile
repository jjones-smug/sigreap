CC = gcc -Wall -g
LDFLAGS =

default :
	@echo "You can make 'sigreap', 'all', or 'clean'"

.PHONY: all clean

all : sigreap

sigreap.o : sigreap.c Makefile
	$(CC) -c -o $@ $(CFLAGS) $<

sigreap : sigreap.o Makefile
	$(CC) -o $@ $(@).o $(LDFLAGS)

clean :
	@test -f sigreap && rm sigreap || true
	@test -f sigreap.o && rm sigreap.o || true
