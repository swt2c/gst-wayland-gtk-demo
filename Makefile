CC=gcc
CFLAGS=-Wall `pkg-config --cflags --libs gtk+-3.0 cairo gtk+-wayland-3.0 gstreamer-video-1.0 wayland-client`

debug:clean
	$(CC) $(CFLAGS) -g -o gst-wayland-gtk-demo main.c
stable:clean
	$(CC) $(CFLAGS) -o gst-wayland-gtk-demo main.c
clean:
	rm -vfr *~ gst-wayland-gtk-demo
