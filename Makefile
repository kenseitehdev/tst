CC = clang
CFLAGS = -Wall -Wextra -O2 -std=c99
TARGET = ./bin/tst
PREFIX = /usr/local

all: $(TARGET)

$(TARGET): src/main.c
	$(CC) $(CFLAGS) -o $(TARGET) src/main.c

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/

uninstall:
	rm -f $(PREFIX)/bin/$(TARGET)

.PHONY: all clean install uninstall