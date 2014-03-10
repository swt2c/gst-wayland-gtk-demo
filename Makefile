CC=gcc
CFLAGS=-Wall `pkg-config --cflags --libs gtk+-3.0 gtk+-wayland-3.0 gstreamer-video-1.0 gstreamer-plugins-bad-1.0 wayland-client` -lgstwayland-1.0

debug:clean
	$(CC) $(CFLAGS) -g -o gst-gtk-demo main.c
stable:clean
	$(CC) $(CFLAGS) -o gst-gtk-demo main.c
clean:
	rm -vfr *~ gst-gtk-demo
