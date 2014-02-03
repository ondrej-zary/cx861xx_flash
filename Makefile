all: cx861xx_flash.c
	gcc -std=c99 -Wall -Wextra -lusb-1.0 cx861xx_flash.c -o cx861xx_flash
clean:
	rm -f cx861xx_flash
