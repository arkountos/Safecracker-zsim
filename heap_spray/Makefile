all:
	gcc -Wno-format ./server.c ../common/cache_manipulation.c -std=c11 -o ./server
	gcc ./client.c -std=c11 -o ./client
	gcc ./attacker.c ../common/cache_manipulation.c ../bdi_exploit/steal_bytes.c -std=c11 -o ./attacker
clean:
	rm ./client ./server ./attacker
