all:
	make -C heap_spray
	make -C buffer_overflow
clean:
	make -C heap_spray clean
	make -C buffer_overflow clean
