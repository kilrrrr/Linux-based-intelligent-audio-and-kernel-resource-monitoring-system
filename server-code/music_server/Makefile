objects = main.o player.o server.o
main: $(objects)
	g++ -o main $(objects) -levent -ljson-c
main.o: main.cpp
	g++ -c main.cpp
player.o: player.cpp
	g++ -c player.cpp
server.o: server.cpp
	g++ -c server.cpp

	
clean:
	rm *.o
	rm main
