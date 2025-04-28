build:
	$(CC) main.c -o osmgrd -lcrossdb -lpthread -g

run: all
	./osmgrd

install: all
	@mkdir -p /sonic-ng/bin
	install -c osmgrd /sonic-ng/bin/
