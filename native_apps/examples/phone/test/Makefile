CC ?= cc
CFLAGS += -std=gnu2x -Wall -Wextra -Werror -g -fsanitize=address,undefined
CFLAGS += -I. -I../main -I../../../include -I../../../../src

APP = ../main/book.c ../main/ui.c
TESTS = test_main.c fakes.c test_book.c test_ui.c test_phone.c

.PHONY: test clean
test: phone_test
	./phone_test

phone_test: $(TESTS) $(APP) ../main/phone.c fakes.h test.h
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(APP) $(LDLIBS)

clean:
	rm -f phone_test
