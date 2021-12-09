#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include "interface.h"
#include "../common/constants.h"
#include "../common/cache_manipulation.h"

#include "../common/zsim_hooks.h"

unsigned char* ATTACKER_ADDR;
unsigned char* KEYADDR;
long  KEYSET;
long  KEYOFFSET;
int SECRETSIZE;
unsigned char buffer[BUFFERSIZE];

char *last;

void nothing(){}

void printline(){
  char * base = last - ((long) last % LINESIZE);
    printf("[S] Line of the key: [");
    for(int i = 0; i < LINESIZE; i++) {
      printf("%i,", base[i-KEYOFFSET]);
    }
    printf("]\n");
}

void set_secret(unsigned char *secret);
void copy_data(char *attacker_text, int len, int offset);
void encrypt(char *plaintext);
void write_around_key(char *buffer);

void initialize_key(){
  KEYADDR = malloc(SECRETSIZE);

  for(int x = 0; x < SECRETSIZE; x++){
    KEYADDR[x] = 155;
  }

  printf("[S] Key is: ");
  for(int x = 0; x < SECRETSIZE; x++){
    printf("%u,", KEYADDR[x]);
  }
  printf("\n");
}

void accessKey(){
  int tmp = 0;
  for (int i = 0; i < 4; i++) {
    tmp += last[i];
  }
}

void main(int argc, char *argv[]){
  SECRETSIZE = argc > 1 ? atoi(argv[1]) : 4;
  if(SECRETSIZE == 0) {
    zsim_heartbeat();
    exit(EXIT_SUCCESS);
  }

  unlink(SOCKET_NAME);

  ATTACKER_ADDR = malloc(100);
  char *garbage = malloc(10000);
  initialize_key();

  encrypt(&buffer[1]);

  KEYSET = ((long) KEYADDR / LINESIZE) % NUM_SETS;
  KEYOFFSET = ((long) KEYADDR) % LINESIZE;
  printf("[S] Offset of the key on cache line: %ld\n", KEYOFFSET);
  printf("[S] Set where the key is: %ld\n", KEYSET);
  printf("[S] KEYADDR = %x\n", KEYADDR);
  printf("[S] SECRETSIZE = %i\n", SECRETSIZE);
  printf("[S] Necessary attacker offset: %ld\n", KEYADDR-ATTACKER_ADDR);

  
  int sock = socket(AF_LOCAL, SOCK_SEQPACKET, 0);

  struct sockaddr_un addr_socket; //Asginarle dirección
  unsigned int size_socket = sizeof(addr_socket);
  bzero(&addr_socket, size_socket);
  addr_socket.sun_family = AF_UNIX;

  strncpy(addr_socket.sun_path, SOCKET_NAME, sizeof(addr_socket.sun_path) - 1);
  
  // Assign a name to the socket
  int biU = bind(sock, (struct sockaddr *) &addr_socket, size_socket);

  // Prepare for accepting connections. The backlog size is set to 20
  listen(sock, 20);
  int terminate = 0;

  int data_socket, _r;

  zsim_roi_begin();

  while(!terminate) { // Loop for accepting connections (attacker and victim)
    data_socket = accept(sock, NULL, NULL);
    int stop_processing = 0;
    while(!terminate && !stop_processing) { // Loop for process queries
      _r = read(data_socket, buffer, BUFFERSIZE);
      // Process queries to call other functions
      switch(buffer[0]) {
      case SET_KEY:;
	set_secret(&buffer[1]);
	break;
      case WRITE_BUFFER:;
	int *offset = (int *) &buffer[2];
	copy_data(&buffer[6], (int) buffer[1], *offset);
	break;
      case WRITE_AROUND_KEY:;
	write_around_key(&buffer[1]);
	break;
      case CIPHER:
	encrypt(&buffer[1]);
	break;
      case GET_EVICTED_LINES:; // 1st put here the pressure (prime+probe), 2nd in attacker side
	bzero(buffer, BUFFERSIZE);
	unsigned *size_cache_line = (unsigned *) buffer;
	*size_cache_line = get_size(KEYSET, &accessKey, &nothing);
	_r = write(data_socket, buffer, BUFFERSIZE);
	break;
      case ACCESS_KEY:;
	accessKey();
	bzero(buffer, BUFFERSIZE);
	_r = write(data_socket, buffer, BUFFERSIZE);
	break;
      case DUMMY_ACCESS:;
	bzero(buffer, BUFFERSIZE);
	_r = write(data_socket, buffer, BUFFERSIZE);
	break;
      case FLUSH:
	FlushSet(((long) last / LINESIZE) % NUM_SETS);
	_r = write(data_socket, buffer, BUFFERSIZE);
	break;
      case PRINT:
	printline();
	break;
      case GET_SET:
      	bzero(buffer, BUFFERSIZE);
      	long *key_set = (long *) buffer;
	*key_set = ((long) last / LINESIZE) % NUM_SETS;
      	_r = write(data_socket, buffer, BUFFERSIZE);
      	break;
      case KILL:;
	terminate = 1;
      case END:;
      default :
	stop_processing = 1;
      }
    }
    close(data_socket);
  }
  
  close(sock);
  /* Unlink the socket. */
  unlink(SOCKET_NAME);
  printf("[S] Program terminated\n");

  zsim_heartbeat();
      
  exit(EXIT_SUCCESS);
}

void set_secret(unsigned char *secret){
  memcpy(KEYADDR, secret, SECRETSIZE);
  printf("[S] Secret set with value ");
  for(int i = 0; i < SECRETSIZE; i++) printf("%i,", secret[i]);
  printf("\n");
}

void copy_data(char *buffer, int len, int offset){
  memcpy(ATTACKER_ADDR+offset, buffer, len);
}

void write_around_key(char *buffer){
  // Writes before the key
  memcpy(KEYADDR-KEYOFFSET, &buffer[LINESIZE-SECRETSIZE-KEYOFFSET], KEYOFFSET);
  // Writes after the key
  memcpy(KEYADDR+SECRETSIZE, buffer, LINESIZE-SECRETSIZE-KEYOFFSET);
}

void my_strcpy(char *dest, char *src){ // More srtict version, instead of finishing on 1 zero, there must be 8 of them
  int i = 0;
  while (src[i] || src[i+1] || src[i+2] || src[i+3] || src[i+4] || src[i+5] || src[i+6] || src[i+7]){
    dest[i] = src[i];
    i++;
  }
}

// This function receives the plaintext as argument, which is placed
// in a local temporary buffer. A later buffer contains the key (with
// a `garbage` buffer in between), the plaintext may cause an stack
// overflow (due to my_strcpy function), so it would rewrite the copy
// of the key
void encrypt(char *plaintext) {

  unsigned char text_encrypted[LINESIZE] __attribute__( ( aligned ( LINESIZE ) ) );
  char garbage[LINESIZE]  __attribute__( ( aligned ( LINESIZE ) ) );
  garbage[0] = 1; // To avoid compiler optimizations that drop this region
  unsigned char key[LINESIZE] __attribute__( ( aligned ( LINESIZE ) ) );

  key[0] ^= 0; // TODO fix this on simulator side, for geting the right compressed size
  
  memcpy(key+LINESIZE-SECRETSIZE, KEYADDR, SECRETSIZE);
  my_strcpy(text_encrypted, plaintext);
    
  last = key+LINESIZE-SECRETSIZE;
  
  for (int i = 0; i < LINESIZE; i++){
    text_encrypted[i] ^= key[i % SECRETSIZE];
  }
}
