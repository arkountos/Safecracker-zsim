int PrimeSet(long set);
int FlushSet(long set);
int ProbeSet(long set); // return how many misses the set has
void PrimeSetWithSize(long set, int size);
int PrimeSets(int nsets);
int FlushSets(int nsets);
int get_evicted_lines(int set, void(*accessSecret)(), void(*flush)());
void initialize_data();
int get_set(void (*flush)(), void (*accessKey)(), void (*dummyAccess)());
int get_size(int set, void(*accessSecret)(), void(*flush)());
