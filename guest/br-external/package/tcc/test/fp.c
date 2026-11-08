#include <stdio.h>
#include <math.h>
static double hyp(double x, double y) { return sqrt(x * x + y * y); }
static float halve(float f) { return f / 2; }
int main(void) {
	double d = 1.0 / 3.0, s = 0;
	float f = 2.5f;
	int i;
	for (i = 1; i <= 1000; i++) s += 1.0 / ((double)i * i);
	printf("d %.17g f %f halve %g\n", d, f * 3, halve(f));
	printf("hyp %.6f pi^2/6 %.12f sum %.12f\n", hyp(3, 4), M_PI * M_PI / 6, s);
	printf("sin %.6f exp %.6f pow %.3e\n", sin(1.0), exp(2.0), pow(2.0, 100));
	printf("conv %d %u %lld %.1f %.1f\n", (int)-3.7, (unsigned)3.9e9, (long long)-1e15, (double)(1LL << 53), (float)7);
	printf("cmp %d %d %d nan %d\n", d < 0.5, f >= 2.5f, -d > d, NAN == NAN);
	printf("ld %Lf %Lg\n", (long double)d * 3, 1e300L);
	return 0;
}
