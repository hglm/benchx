all: benchx

benchx : benchx.c
	gcc -O2 -std=c99 -lX11 -lXext -lXrender -lXcomposite -lrt benchx.c -o benchx

install : benchx
	install -m 0755 benchx /usr/bin/benchx

clean :
	rm -f benchx
