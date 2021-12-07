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
// -----
// My additions to set a fixed key for testing
int SECRETVALUE;
// -----
unsigned char buffer[BUFFERSIZE];

void nothing(){}

unsigned char* int_to_bytes(int input){
	static unsigned char arr[2];
	arr[1] = (unsigned)input & 0xff;
	arr[0] = (unsigned)input >> 8;
	printf("[TESTING] The two values: %d, %d\n", arr[0], arr[1]);
	//printf("[S] From %d, we get %d and %d.", input, atoi(msb), atoi(lsb) );
	return arr;
}


void printline(){
    printf("[S] Line of the key: [");
    for(int i = 0; i < LINESIZE; i++) {
      printf("%i,", KEYADDR[i-KEYOFFSET]);
    }
    printf("]\n");
}

void set_secret(unsigned char *secret);
void copy_data(char *attacker_text, int len, int offset);
void cipher();
void write_around_key(char *buffer);

/* This initializes the key address, not the value. The value is set with set_secret! */
void initialize_key(int s){
  KEYADDR = malloc(SECRETSIZE);

    for(int x = 0; x < SECRETSIZE; x++){
    KEYADDR[x] = (x*183+41)%256;
  }
  
  /*printf("[S] Key is: ");
  for(int x = 0; x < SECRETSIZE; x++){
    printf("%u,", KEYADDR[x]);
  }
  printf("\n");
  */
  // --------
  /*printf("[S] But now I call set_secret!\n");
  char c1 = (char)s1;
  char c2 = (char)s2;
  char* input_secret;
  input_secret = (char*)malloc(SECRETSIZE*sizeof(char));
  input_secret[0] = c1;
  input_secret[1] = c2;
  printf("[S] Built buffer is: %i, %u=i\n", input_secret[0], input_secret[1]);
  */
  unsigned char* input_secret = int_to_bytes(s);
  unsigned char input_array[2];
  input_array[0] = *input_secret;
  input_array[1] = *(input_secret+1);
  printf("[S] Input array is: %d, %d\n", (int)input_array[0], (int)input_array[1]);
  set_secret(input_array);
}

void accessKey(){
  int tmp = 0;
  for (int i = 0; i < 4; i++) {
    tmp += KEYADDR[i];
  }
}

void main(int argc, char *argv[]){
  //SECRETSIZE = argc > 1 ? atoi(argv[1]) : 4;
  SECRETSIZE = atoi(argv[1]);
  SECRETVALUE = atoi(argv[2]);
  
  printf("[S] Passes argument input.");

  unlink(SOCKET_NAME);

  ATTACKER_ADDR = malloc(100);
  char *garbage = malloc(10000);
  initialize_key(SECRETVALUE);

  KEYSET = ((long) KEYADDR / LINESIZE) % NUM_SETS;
  KEYOFFSET = ((long) KEYADDR) % LINESIZE;
  printf("[S] Offset of the key on cache line: %ld\n", KEYOFFSET);
  printf("[S] Set where the key is: %ld\n", KEYSET);
  printf("[S] KEYADDR = %x\n", KEYADDR);
  printf("[S] SECRETSIZE = %i\n", SECRETSIZE);
  
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
	cipher();
	break;
      case GET_EVICTED_LINES:; // 1st put here the pressure (prime+probe), 2nd in attacker side
	bzero(buffer, BUFFERSIZE);
	unsigned *size_cache_line = (unsigned *) buffer;
	// Resulting compressed size is sizes[lo]
	*size_cache_line = get_evicted_lines(KEYSET, &accessKey, &nothing);
	_r = write(data_socket, buffer, BUFFERSIZE);
	break;
      case ACCESS_KEY:; // 1st put here the pressure (prime+probe), 2nd in attacker side
	cipher();
	bzero(buffer, BUFFERSIZE);
	_r = write(data_socket, buffer, BUFFERSIZE);
	break;
      case DUMMY_ACCESS:; // 1st put here the pressure (prime+probe), 2nd in attacker side
	bzero(buffer, BUFFERSIZE);
	_r = write(data_socket, buffer, BUFFERSIZE);
	break;
      case FLUSH:
	FlushSet(KEYSET);
	_r = write(data_socket, buffer, BUFFERSIZE);
	break;
      case PRINT:
	printline();
	break;
      case GET_SET:
      	bzero(buffer, BUFFERSIZE);
      	long *key_set = (long *) buffer;
      	*key_set = KEYSET;
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
  //printf("[S] Reaches here!\n");
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
  /* memcpy(KEYADDR-LINESIZE, buffer, LINESIZE); */
  memcpy(KEYADDR-KEYOFFSET, &buffer[LINESIZE-SECRETSIZE-KEYOFFSET], KEYOFFSET);
  // Writes after the key
  /* memcpy(KEYADDR+SECRETSIZE, buffer, LINESIZE-SECRETSIZE); */
  memcpy(KEYADDR+SECRETSIZE, buffer, LINESIZE-SECRETSIZE-KEYOFFSET);
}

void cipher() {
  int key = (int) *KEYADDR;
  for (int i = 0; i < LINESIZE; i+=4){ // 4 bytes key
    int *to_encrypt = (int *) &ATTACKER_ADDR[i];
    *to_encrypt ^= key;
  }
}
