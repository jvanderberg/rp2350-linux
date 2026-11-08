#include <stdio.h>
#include "vec.h"
int main(void) {
	int a[] = { 100000, 200000, 300000 }, b[] = { 400000, 500000, 600000 };
	printf("dot %lld mean %.3f\n", vdot(a, b, 3), vmean(a, 3));
	return 0;
}
