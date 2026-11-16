CC ?= cc
CFLAGS += -std=gnu2x -Wall -Wextra -Werror -g -fsanitize=address,undefined
CFLAGS += -I. -I../main -I../../../include -I../../../../src
LDLIBS += -lm

APP = ../main/geo.c ../main/map_view.c ../main/tiles.c ../main/hud.c ../main/settings.c ../main/search.c
TESTS = test_main.c fakes.c test_geo.c test_map_view.c test_tiles.c test_settings.c test_hud.c test_search.c test_maps.c

.PHONY: test clean
test: maps_test
	./maps_test

maps_test: $(TESTS) $(APP) fakes.h test.h
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(APP) $(LDLIBS)

clean:
	rm -f maps_test
