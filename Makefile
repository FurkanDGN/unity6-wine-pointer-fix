CC = x86_64-w64-mingw32-gcc
CFLAGS = -shared -O2 -Wall -static-libgcc

version.dll: version_ptrfix.c version.def
	$(CC) $(CFLAGS) -o $@ version_ptrfix.c version.def -luser32

clean:
	rm -f version.dll
