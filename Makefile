CFLAGS = -g -Wall -Wextra -pedantic -std=c99
LDFLAGS = -lv4l2
CC ?= gcc
SOURCES := $(wildcard *.c)
OBJECTS := $(addprefix .obj/,$(notdir $(SOURCES:.c=.o)))
OBJECTS_PATH := .obj
TARGET = v4l2grab

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $< -o $@

$(OBJECTS): | $(OBJECTS_PATH)
$(OBJECTS_PATH):
	if [ ! -d $@ ]; then mkdir -p $@; fi

.obj/%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

check: $(SOURCES)
	cppcheck --enable=all .

clean:
	rm -rf .obj $(TARGET)
