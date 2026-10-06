# Host-тесты shproto — обычный gcc/clang, без ESP-IDF.
# Компонент components/shproto зависит только от stdint/stddef/stdbool/string,
# поэтому конечный автомат протокола тестируется на хосте без железа.
#
#   make test   — собрать и прогнать (ждём "ALL PASSED", код возврата 0)
#   make asan   — то же под AddressSanitizer + UBSan (ловит запись за границы)
#   make clean  — убрать бинарники

CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -I../../components/shproto/include -I../../main
SRCS = test_shproto.c test_roundtrip.c ../../components/shproto/shproto.c \
       test_net_time.c ../../main/net_time.c

test: $(SRCS) test_util.h
	$(CC) $(CFLAGS) -o test_runner $(SRCS)
	./test_runner

# Дополнительный прогон под санитайзерами: даже если canary-проверка пропустит
# регрессию с записью за границу буфера, ASan поймает её жёстко.
asan: $(SRCS) test_util.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o test_runner_asan $(SRCS)
	./test_runner_asan

clean:
	rm -f test_runner test_runner_asan

.PHONY: test asan clean
