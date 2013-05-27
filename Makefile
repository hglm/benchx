all: benchx benchxcomp

benchx : benchx.c font_8x8.c font.h
	gcc -O2 -std=gnu99 -lX11 -lXext -lXrender -lXcomposite -lrt benchx.c font_8x8.c -o benchx

benchxcomp : benchxcomp.c
	gcc -O2 -std=gnu99 benchxcomp.c -o benchxcomp

install : benchx benchxcomp
	install -m 0755 benchx /usr/bin/benchx
	install -m 0755 benchxcomp /usr/bin/benchxcomp

clean :
	rm -f benchx
	rm -f benchxcomp
