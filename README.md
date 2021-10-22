# How to run 

1. Build zsim:
We provide a customized zsim (pin-based multi-core simulator), which also
builds on top of the simulator in MaxSim (https://github.com/arodchen/MaxSim).
Nonetheless, most of the MaxSim features are not needed for our experiments.
The important features here are the compressed cache architecture
`compressed_cache_array.*` and the hardware compression algorithm
`bdi_compression.cpp`.
    
See zsim_simulator/README.md for installation instructions.

Also, see https://github.com/s5z/zsim for other zsim questions.

2. Build attack PoC:

Generate the binaries by executing `make` at the project root folder
To test either the PoC 1 (Static colocation) or the Poc 2 (Buffer
overflow-based attack), run `zsim` using the respective configuration
files as an argument (be sure to execute it into the respective
folder):

`zsim heap_spray.cfg` or `zsim buffer_overflow.cfg`

The programs server and receiver receive as a first argument the
secret size (in bytes), it's 4 by default if it isn't provided.

# Source code structure

## bdi_exploit

`steal_bytes.c` file contains the algorithms for cracking
from 1 to 8 bytes both with heap spraying (function `steal`) or
buffer_overflow (function `steal_incremental`). This is no more than
the concrete implementation of Sec. 3.3

## common

There are header files that defines variables related with the system
simulation. `steal_bytes.c` file contains the algorithms
implementation for cache pressure techniques (with a BDI compressed
cache).

### Functions:
Further than the `Prime` and `Probe` functions, there are also implemented the next ones:

- `get_size_LRU`: Executes *Pack+Probe* (for BDI algorithm) assuming a
  compressed cache with LRU replacement policy.
  
- `get_size`: Executes *Pack+Probe* (for BDI algorithm) by performing
  a binary search (which is slower than `get_size`).

- `PrimeSetWithSize`: It fills the set, letting free the number of
  bytes received as argument.

- `get_set`: By a classic *Prime+Probe* technique, returns the set
  accessed when executed the function passed as argument. Additionaly,
  it receives another function that lets to drop the coincidences
  obtained when it's executed.

## heap_spray
The code for the server and attacker to perform the PoC 1 (Sec.5.2).

## buffer_overflow
The code for the server and attacker to perform the PoC 2 (Sec.5.3).

Although the code is compiled tih `-fstack-protector`, the attacker is
able to overflow the stack to perform the exploit without producing
any crash.

In this case the server gives the set directly to the attacker because
we've demonstrated the effectivity of the Prime+Probe tachnique on PoC
1, reducing the total execution time this way.

## Annotations

The socket setup implementation was taken from the GNU man pages
(example of `man 2 bind`) and later modified to match our
communication specification.
