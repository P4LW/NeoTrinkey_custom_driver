obj-m += NeoTrinkey_custom_driver.o

CC ?= gcc
CFLAGS ?= -O2 -Wall

USER_APPS = trinkey_app trinkeyctl trinkey_sub_logger

all: modules user

modules:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

user: $(USER_APPS)

trinkey_app: trinkey_app.c trinkey_nl.h
	$(CC) $(CFLAGS) trinkey_app.c -o trinkey_app -lm

trinkeyctl: trinkeyctl.c
	$(CC) $(CFLAGS) trinkeyctl.c -o trinkeyctl

trinkey_sub_logger: trinkey_sub_logger.c trinkey_nl.h
	$(CC) $(CFLAGS) trinkey_sub_logger.c -o trinkey_sub_logger

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f $(USER_APPS)

.PHONY: all modules user clean
