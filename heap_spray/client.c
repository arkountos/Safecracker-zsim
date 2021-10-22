#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include "interface.h"

int main(int argc, char *argv[]){
    struct sockaddr_un addr;
    int i;
    int ret;
    int data_socket;
    unsigned char buffer[BUFFERSIZE];

    /* Create local socket. */

    data_socket = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (data_socket == -1) {
        perror("socket");
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
    strncpy(addr.sun_path, SOCKET_NAME, sizeof(addr.sun_path) - 1);

    ret = connect (data_socket, (const struct sockaddr *) &addr,
                   sizeof(struct sockaddr_un));
    if (ret == -1) {
        fprintf(stderr, "The server is down.\n");
        exit(EXIT_FAILURE);
    }

    /* Send arguments. */
    bzero(buffer, BUFFERSIZE);
    for (i = 1; i < argc && i < BUFFERSIZE; ++i) {
      buffer[i-1] = atoi(argv[i]);    
    }
    printf("Sending %i...\n", buffer[0]);
    int _r = write(data_socket, buffer, BUFFERSIZE);

    bzero(buffer, BUFFERSIZE);
    buffer[0] = END; // Necessary every time 
    _r = write(data_socket, buffer, BUFFERSIZE);

    close(data_socket);    
    exit(EXIT_SUCCESS);
}
