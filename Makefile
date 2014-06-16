CC=gcc
CFLAGS=-Wall `pkg-config --cflags --libs gtk+-3.0 cairo gtk+-wayland-3.0 gstreamer-wayland-1.0 wayland-client`

debug:clean
	$(CC) $(CFLAGS) -g -o gst-gtk-demo main.c
stable:clean
	$(CC) $(CFLAGS) -o gst-gtk-demo main.c
clean:
	rm -vfr *~ gst-gtk-demo
