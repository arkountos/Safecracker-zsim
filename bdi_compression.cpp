/* Taken from https://github.com/CMU-SAFARI/BDICompression

   Credit:

Base-Delta-Immediate Compression Algorithm

Author: Gennady Pekhimenko

Affiliation: Carnegie Mellon University

Description:

This repository provides the source code for the compression algorithm described in our following PACT 2012 paper. If you use this code as part of your evaluation in your research/development, please be sure to cite the following paper for proper acknowledgment:

Gennady Pekhimenko, Vivek Seshadri, Onur Mutlu, Philip B. Gibbons, Michael A. Kozuch, and Todd C. Mowry, "Base-Delta-Immediate Compression: Practical Data Compression for On-Chip Caches" Proceedings of the 21st ACM International Conference on Parallel Architectures and Compilation Techniques (PACT), Minneapolis, MN, September 2012.

Paper reference: http://www.cs.cmu.edu/~gpekhime/Papers/pact12_pekhimenko.pdf

Slides reference: http://www.cs.cmu.edu/~gpekhime/Talks/BDI-Compression.pptx

 */

//#include <iostream>
//#include <vector>
#include <stdlib.h>  
#include <stdio.h>
#include <time.h>       /* for srand in main */
#include <string.h>     /* for strtol in main */
// #include "log.h"
//void test (void) {
//     std::cout << "test" << std::endl;

//}
using namespace std;

static unsigned long long my_llabs ( long long x )
{
   unsigned long long t = x >> 63;
   return (x ^ t) - t;
}

static unsigned my_abs ( int x )
{
   unsigned t = x >> 31;
   return (x ^ t) - t;
}

#define VG_(x) x

long long unsigned * convertBuffer2Array (long long unsigned* values, char * buffer, unsigned size, unsigned step)
{
      /* Don't malloc! -nzb */
    
      /* long long unsigned * values = (long long unsigned *) VG_(malloc)("cg.compress.ci.1", sizeof(long long unsigned) * size/step); */
//      std::cout << std::dec << "ConvertBuffer = " << size/step << std::endl;
     //init
    unsigned int i, j;
     for (i = 0; i < size / step; i++) {
          values[i] = 0;    // Initialize all elements to zero.
      }
      // printf("[CBA] Element Size (step) = %d \n", step);
      for (i = 0; i < size; i += step ){
          for (j = 0; j < step; j++){
            //   printf("[CBA] Buffer (buffer[%d + %d]) = %02x \n", i, j, (unsigned char) buffer[i + j]);
              values[i / step] += (long long unsigned)((unsigned char)buffer[i + j]) << (8*j);
              //long long unsigned val = ((unsigned char)buffer[i + j]) << (8*j);
              //values[i / step] += val;
              //if (j >= 4 && buffer[i + j] !=0 && val < 1llu << 32 ) {
              //    info("j %u buffer[i + j] %u val %llu", j, (unsigned char) buffer[i + j], val);
              //    //panic("Damn it nzb");
              //  }
             //SIM_printf("step %d value = ", j);
              //printLLwithSize(values[i / step], step);  
          }
          //std::cout << "Current value = " << values[i / step] << std::endl;
          //printLLwithSize(values[i / step], step);
          //SIM_printf("\n");
      }
      // std::cout << "End ConvertBuffer = " << size/step << std::endl;
      printf("End CovertBuffer = %d\n", size/step);
      return values;
}

///
/// Check if the cache line consists of only zero values
///
int isZeroPackable ( long long unsigned * values, unsigned size){
  int nonZero = 0;
  unsigned int i;
  for (i = 0; i < size; i++) {
      if( values[i] != 0){
          nonZero = 1;
          break;
      }
  }
  return !nonZero;
}

///
/// Check if the cache line consists of only same values
///
int isSameValuePackable ( long long unsigned * values, unsigned size){
  int notSame = 0;
  unsigned int i;
  for (i = 0; i < size; i++) {
      if( values[0] != values[i]){
          notSame = 1;
          break;
      }
  }
  return !notSame;
}

///
/// Check if the cache line values can be compressed with multiple base + 1,2,or 4-byte offset 
/// Returns size after compression 
///
unsigned doubleExponentCompression ( long long unsigned * values, unsigned size, unsigned blimit, unsigned bsize){
  unsigned long long limit = 0;
  //define the appropriate size for the mask
  switch(blimit){
    case 1:
      limit = 56;
      break;
    case 2:
      limit = 48;
      break;
    default:
      // std::cout << "Wrong blimit value = " <<  blimit << std::endl;
      VG_(exit)(1);
  }
  // finding bases: # BASES
  // find how many elements can be compressed with mbases
  unsigned compCount = 0;
  unsigned int i;
  for (i = 0; i < size; i++) {
         if( (values[0] >> limit) ==  (values[i] >> limit))  {
             compCount++;
         }
  }
  //return compressed size
  if(compCount != size )
     return size * bsize;
  return size * bsize - (compCount - 1) * blimit;
}


///
/// Check if the cache line values can be compressed with multiple base + 1,2,or 4-byte offset 
/// Returns size after compression 
///
unsigned multBaseCompression ( long long unsigned * values, unsigned size, unsigned blimit, unsigned bsize){
  unsigned long long limit = 0;
  unsigned BASES = 1;
  //define the appropriate size for the mask
  switch(blimit){
    case 1:
      limit = 0xFF;
      break;
    case 2:
      limit = 0xFFFF;
      break;
    case 4:
      limit = 0xFFFFFFFF;
      break;
    default:
      //std::cout << "Wrong blimit value = " <<  blimit << std::endl;
      VG_(exit)(1);
  }
  // finding bases: # BASES
  //std::vector<unsigned long long> mbases;
  //mbases.push_back(values[0]); //add the first base
  unsigned long long mbases [64];
  unsigned baseCount = 1;
  mbases[0] = 0;
  unsigned int i,j;
  for (i = 0; i < size; i++) {
      for(j = 0; j <  baseCount; j++){
         if( my_llabs((long long int)(mbases[j] -  values[i])) > limit ){
             //mbases.push_back(values[i]); // add new base
             mbases[baseCount++] = values[i];  
         }
     }
     if(baseCount >= BASES) //we don't have more bases
       break;
  }
  // find how many elements can be compressed with mbases
  unsigned compCount = 0;
  for (i = 0; i < size; i++) {
      //ol covered = 0;
      for(j = 0; j <  baseCount; j++){
         if( my_llabs((long long int)(mbases[j] -  values[i])) <= limit ){
             compCount++;
             break;
         }
     }
  }
  //return compressed size
  unsigned mCompSize = blimit * compCount + bsize * BASES + (size - compCount) * bsize;
  if(compCount < size)
     return size * bsize;
  //VG_(printf)("%d-bases bsize = %d osize = %d CompCount = %d CompSize = %d\n", BASES, bsize, blimit, compCount, mCompSize);
  //printf("%d-bases bsize = %d osize = %d CompCount = %d CompSize = %d\n", BASES, bsize, blimit, compCount, mCompSize);

  return mCompSize;
}

unsigned BDICompress (char * buffer, unsigned _blockSize)
{
  //char * dst = new char [_blockSize];
//  print_value(buffer, _blockSize);

    long long unsigned array[ _blockSize * 8 ];
 
  long long unsigned * values = convertBuffer2Array( array, buffer, _blockSize, 8);
  unsigned bestCSize = _blockSize;
  unsigned currCSize = _blockSize;
  if( isZeroPackable( values, _blockSize / 8))
      bestCSize = 1;
  if( isSameValuePackable( values, _blockSize / 8))
      currCSize = 8;
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize = multBaseCompression( values, _blockSize / 8, 1, 8);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize = multBaseCompression( values, _blockSize / 8, 2, 8);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize =  multBaseCompression( values, _blockSize / 8, 4, 8);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  // VG_(free)(values);
  values = convertBuffer2Array( array, buffer, _blockSize, 4);
  if( isSameValuePackable( values, _blockSize / 4))
      currCSize = 4;
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize = multBaseCompression( values, _blockSize / 4, 1, 4);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize = multBaseCompression( values, _blockSize / 4, 2, 4);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  // VG_(free)(values);
  values = convertBuffer2Array( array, buffer, _blockSize, 2);
  currCSize = multBaseCompression( values, _blockSize / 2, 1, 2);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  // VG_(free)(values);

  //exponent base compression
  /*values = convertBuffer2Array( array, buffer, _blockSize, 8);
  currCSize = doubleExponentCompression( values, _blockSize / 8, 2, 8);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  currCSize = doubleExponentCompression( values, _blockSize / 8, 1, 8);
  bestCSize = bestCSize > currCSize ? currCSize: bestCSize;
  VG_(free)(values);*/
 
  //delete [] buffer;
  buffer = NULL;
  values = NULL;
  //SIM_printf(" BestCSize = %d \n", bestCSize);
  return bestCSize;

}

unsigned FPCCompress(char * buffer, unsigned size ){
   long long unsigned array[ size * 4 * 8 ];
   long long unsigned * values = convertBuffer2Array(array, buffer, size*4, 4);
   unsigned compressable = 0;
   unsigned int i;
   for (i = 0; i < size; i++) {
     
     //SIM_printf("c_size = %llx \n", compressable);
     //SIM_printf("V = %llx \n", values[i]);
     // 000
     if(values[i] == 0){
        compressable += 1;//SIM_printf("000\n ");
        continue;
     }
     // 001 010
     if(my_abs((int)(values[i])) <= 0xFF){
        compressable += 1;//SIM_printf("001\n ");
        continue;
     }
     // 011
     if(my_abs((int)(values[i])) <= 0xFFFF){
        compressable += 2;//SIM_printf("011\n ");
        continue;
     }
     //100  
     if(((values[i]) & 0xFFFF) == 0 ){
        compressable += 2;//SIM_printf("100\n ");
        continue;
     }
     //101
     if( my_abs((int)((values[i]) & 0xFFFF)) <= 0xFF
         && my_abs((int)((values[i] >> 16) & 0xFFFF)) <= 0xFF){
        compressable += 2;//SIM_printf("101\n ");
        continue;
     }
     //110
     unsigned byte0 = (values[i]) & 0xFF;
     unsigned byte1 = (values[i] >> 8) & 0xFF;
     unsigned byte2 = (values[i] >> 16) & 0xFF;
     unsigned byte3 = (values[i] >> 24) & 0xFF;
     if(byte0 == byte1 && byte0 == byte2 && byte0 == byte3){
        compressable += 1; //SIM_printf("110\n ");
        continue;
     }
     //111
     compressable += 4;
     //SIM_printf("111\n ");
   }
   // VG_(free)(values);
   //6 bytes for 3 bit per every 4-byte word in a 64 byte cache line
   unsigned compSize = compressable + size * 3 / 8;
   if(compSize < size * 4)
      return compSize;
   else
      return size * 4;
}

unsigned GeneralCompress (char * buffer, unsigned _blockSize, unsigned compress)
{   // compress is the actual compression algorithm
   switch (compress)
   {
        case 0:
           return _blockSize;
           break; 
        case 1:	
   	   return BDICompress(buffer, _blockSize);
           break;
        case 2:
           //std::cout << "block-size: " << _blockSize << "\n"; 
           return FPCCompress(buffer, _blockSize/4);
           break;
        case 3:
        {
           unsigned BDISize = BDICompress(buffer, _blockSize);
           unsigned FPCSize = FPCCompress(buffer, _blockSize/4);
           if(BDISize <= FPCSize)
              return BDISize;
	   else
              return FPCSize;
           break;
        }
        default:
         //  cout << "Unknown compression code: " << compress << "\n";
          VG_(exit)(1);         	
   }
}

int main(){
   printf("Hello World!\n");
   // char buffer[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x09};
   // char buffer[] = {0x00000000, 0x0000000B, 0x00000003, 0x00000001, 0x00000004, 0x00000000, 0x00000003, 0x00000009};
   // char buffer[] = {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000};
   // char buffer[32] = "0000000B00000001000400000030009";
   // char buffer[64];
   // char buffer[64] = "0000000B000000000004000000000090000000B00000001000000000000009";
   char buffer[65] = "0000000000000009000000030000000100000004000000000000000300000004";
   printf("Sizeof buffer: %lu\n", sizeof(buffer) * sizeof(buffer[0]));
   printf("buffer[0] as %08x, %d, %c\n", buffer[0]);
   printf("buffer[0] as %08x, %d, %c\n", '0');
   printf("buffer[7] as %08x, %d, %c\n", buffer[7]);
   printf("buffer[64] as %08x, %d, %c\n", buffer[64]);
   printf("buffer[65] as %08x, %d, %c\n", buffer[65]);


   srand(time(NULL));
   // for (int j = 0; j < 100; j++){
   // int myints[64] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,4};
   //    for (int i = 0; i < 64; i++){
   //       // buffer[i] = rand() % 25 + 65;
   //       buffer[i] = myints[i];
   //       printf("%d", buffer[i]);
   //    }
   //    printf("\n");
      int compressed_size = BDICompress(buffer, 64);
      // if (compressed_size < 64){   
         printf("\nResult = %d\n", compressed_size);
      // }
   // }
   
   // buffer[35] = 'T';
   // buffer[5] = 'T';
   // printf("\nbuffer[35] = %c\n", buffer[35]);
   // printf("buffer[36] = %c\n", buffer[36]);

   // printf("Calling compression\n");
   // int compressed_size = BDICompress(buffer, 64);
   // printf("Result = %d\n", compressed_size);
   return(0);
}