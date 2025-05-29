LIB_NAME = rpmsg_dma
VERSION = 1.0
MAJOR = 1
OBJ = $(patsubst src/%.c,obj/%.o,$(wildcard src/*.c))
OBJ_DIR = obj
LIB_DIR = lib
EXAMPLE_DIRS = example/*/

CFLAGS = -Wall -fPIC -Iinclude
LDFLAGS += -Wl,--hash-style=gnu -shared -Wl,-soname,lib$(LIB_NAME).so.$(MAJOR)

.PHONY: all dirs lib/lib$(LIB_NAME).so.$(VERSION) example clean install

all: dirs lib/lib$(LIB_NAME).so.$(VERSION) example

lib: dirs lib/lib$(LIB_NAME).so.$(VERSION)

dirs:
	@mkdir -p $(OBJ_DIR) $(LIB_DIR)

obj/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

lib/lib$(LIB_NAME).so.$(VERSION): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^
	# Create symlinks
	ln -sf lib$(LIB_NAME).so.$(VERSION) lib/lib$(LIB_NAME).so.$(MAJOR)
	ln -sf lib$(LIB_NAME).so.$(MAJOR) lib/lib$(LIB_NAME).so

example:
	@for dir in $(EXAMPLE_DIRS); do \
                echo "Building example in $$dir..."; \
                $(MAKE) -C $$dir || exit 1; \
        done

clean:
	rm -rf $(OBJ_DIR) $(LIB_DIR)
	@for dir in $(EXAMPLE_DIRS); do \
                echo "Cleaning example in $$dir..."; \
                $(MAKE) -C $$dir clean; \
        done
