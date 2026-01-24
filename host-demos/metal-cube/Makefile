CC := clang

TARGET := metal-cube
SOURCES := main.m

FRAMEWORKS := -framework Cocoa -framework Metal -framework MetalKit -framework QuartzCore
CFLAGS := -fobjc-arc -O3

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) $(FRAMEWORKS) $(SOURCES) -o $@

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
