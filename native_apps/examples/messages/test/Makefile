CC ?= cc
CFLAGS += -std=gnu2x -Wall -Wextra -Werror -g -fsanitize=address,undefined
CFLAGS += -I. -I../main -I../../../include -I../../../../src

APP = ../main/store.c ../main/ui.c
TESTS = test_main.c fakes.c test_store.c test_ui.c test_messages.c

.PHONY: test clean
test: messages_test
	./messages_test

messages_test: $(TESTS) $(APP) ../main/messages.c fakes.h test.h
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(APP) $(LDLIBS)

clean:
	rm -f messages_test
