CC       = gcc
CFLAGS   = -O2 -msse2 -g

TARGET   = goline.exe

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c