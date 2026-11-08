#include "vec.h"
double vmean(const int *a, int n) { double s = 0; int i; for (i = 0; i < n; i++) s += a[i]; return s / n; }
