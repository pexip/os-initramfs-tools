CFLAGS += -Wall -Wextra -O2

all: unmkinitramfs
clean:
	rm -f unmkinitramfs

.PHONY: all clean
