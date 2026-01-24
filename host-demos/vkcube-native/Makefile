GLSLC ?= /usr/local/bin/glslc

CFLAGS = -I/opt/homebrew/include -framework Cocoa -framework Metal -framework QuartzCore
LDFLAGS = -L/opt/homebrew/lib -lvulkan -Wl,-rpath,/opt/homebrew/lib

TARGET = vkcube_native
SOURCES = vkcube_native.m
SHADERS = cube.vert.spv cube.frag.spv

all: $(TARGET) $(SHADERS)

$(TARGET): $(SOURCES) $(SHADERS)
	clang -o $@ $(SOURCES) $(CFLAGS) $(LDFLAGS)

%.spv: %
	$(GLSLC) $< -o $@

clean:
	rm -f $(TARGET) $(SHADERS)

run: all
	./$(TARGET)

.PHONY: all clean run
