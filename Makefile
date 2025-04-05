ifeq ($(OS),Windows_NT)
    TARGET = goline.exe
    SRC    = src/windows/main.c
    CFLAGS = -O2 -msse2 -g
else
    TARGET = goline
    SRC    = src/linux/main.c
    CFLAGS = -O2 -msse2 -g -std=c99 -Wall
endif

CC = gcc

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)
