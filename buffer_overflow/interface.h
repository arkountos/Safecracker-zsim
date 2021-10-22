#define SET_KEY 0
#define WRITE_BUFFER 1
#define CIPHER 2
#define GET_EVICTED_LINES 3
#define GET_SET 4
#define WRITE_AROUND_KEY 5
#define ACCESS_KEY 6
#define PRINT 7
#define FLUSH 8
#define END 9
#define KILL 10
#define DUMMY_ACCESS 11

#define SOCKET_NAME "/tmp/other.socket"
#define BUFFERSIZE 256

// IDEA: application, set a key, and a buffer, if want to cipher, must end the key again and the result key will be stored on the same buffer (cipher is a XOR)
