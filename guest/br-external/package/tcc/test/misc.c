#include <stdio.h>
#include <stdarg.h>
#include <string.h>
struct pt { int x, y; };
struct big { char name[16]; double v; long long n; };
static struct pt add(struct pt a, struct pt b) { struct pt r = { a.x + b.x, a.y + b.y }; return r; }
static struct big mk(const char *s, double v) { struct big b; strcpy(b.name, s); b.v = v; b.n = 1LL << 40; return b; }
static double sum(int n, ...) {
	va_list ap; double t = 0; int i;
	va_start(ap, n);
	for (i = 0; i < n; i++) t += i & 1 ? va_arg(ap, double) : va_arg(ap, long long);
	va_end(ap);
	return t;
}
static int twice(int x) { return 2 * x; }
static int sq(int x) { return x * x; }
static int apply(int (*f)(int), int v) { return f(v); }
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
#include <stdlib.h>
int main(void) {
	struct pt p = add((struct pt){ 1, 2 }, (struct pt){ 30, 40 });
	struct big b = mk("struct", 2.75);
	int (*fs[])(int) = { twice, sq };
	int v[] = { 5, 3, 9, 1, 7 }, i;
	printf("pt %d %d big %s %.2f %lld\n", p.x, p.y, b.name, b.v, b.n);
	printf("va %.2f\n", sum(4, 10LL, 0.5, 20LL, 0.25));
	printf("fp %d %d\n", apply(fs[0], 21), apply(fs[1], 12));
	qsort(v, 5, sizeof v[0], cmp);
	for (i = 0; i < 5; i++) printf("%d%c", v[i], i < 4 ? ' ' : '\n');
	return 0;
}
