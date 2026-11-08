#include "vec.h"
long long vdot(const int *a, const int *b, int n) { long long s = 0; while (n--) s += (long long)*a++ * *b++; return s; }
