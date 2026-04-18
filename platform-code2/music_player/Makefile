CC = gcc
CFLAGS = -Wall -Wextra -pthread
TARGET = music_player
OBJECTS = main.o device.o socket.o link.o player.o select.o cJSON.o camera.o

$(TARGET): $(OBJECTS)
	$(CC) -o $(TARGET) $(OBJECTS) -pthread

main.o: main.c
	$(CC) $(CFLAGS) -c main.c

device.o: device.c
	$(CC) $(CFLAGS) -c device.c

socket.o: socket.c
	$(CC) $(CFLAGS) -c socket.c

link.o: link.c
	$(CC) $(CFLAGS) -c link.c

player.o: player.c
	$(CC) $(CFLAGS) -c player.c

select.o: select.c
	$(CC) $(CFLAGS) -c select.c

cJSON.o: cJSON.c
	$(CC) $(CFLAGS) -c cJSON.c

camera.o: camera.c
	$(CC) $(CFLAGS) -c camera.c

clean:
	rm -f $(OBJECTS) $(TARGET)
