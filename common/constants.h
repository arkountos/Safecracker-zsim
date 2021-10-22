/* // 2MB cache with 8 ways */
/* #define LINESIZE 64 */
/* #define NUM_SETS 4096 */
/* #define NUM_WAYS 8 */
/* #define CACHE_SIZE LINESIZE*NUM_SETS*NUM_WAYS */
/* #define LLC_LATENCY 40 */
/* #define MEM_LATENCY 140 */

// 8MB cache with 16 ways
#define LINESIZE 64
#define NUM_SETS 8192
#define NUM_WAYS 16
#define CACHE_SIZE LINESIZE*NUM_SETS*NUM_WAYS
#define LLC_LATENCY 80
#define MEM_LATENCY 140
