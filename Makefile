build:
	$(CC) main.c -o osmgrd -lcrossdb -lpthread -g

run: build
	./osmgrd

install: build
	@mkdir -p /sonic-ng/bin
	install -c osmgrd /sonic-ng/bin/
