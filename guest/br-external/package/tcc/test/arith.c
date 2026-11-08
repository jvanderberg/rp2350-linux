/* every operator over a table of values, printed as one hash per line;
   must match the same program built by a native compiler */
#include <stdio.h>
#include <string.h>
typedef long long ll; typedef unsigned long long ull;
static ull H;
static void mix(ull v) { H = (H ^ v) * 0x100000001b3ULL; }
static ll vl[] = { 0, 1, -1, 2, -2, 7, -7, 1000, 0x7fffffff, -0x7fffffff - 1,
	0x80000000LL, 0xffffffffLL, 0x100000000LL, 0x123456789abcdefLL,
	-0x123456789abcdefLL, 0x7fffffffffffffffLL, -0x7fffffffffffffffLL - 1, 3141592653589LL };
static int vi[] = { 0, 1, -1, 3, -3, 31, 100000, -100000, 0x7fffffff, -0x7fffffff - 1 };
static double vd[] = { 0.0, 1.0, -1.0, 0.5, 3.14159, -2.71828, 1e10, -1e-10, 123456789.125, 1e300, -1e-300 };
static float vf[] = { 0.0f, 1.0f, -1.5f, 0.1f, 3.5e7f, -2.25e-5f, 16777217.0f };
#define NL (int)(sizeof vl / sizeof vl[0])
#define NI (int)(sizeof vi / sizeof vi[0])
#define ND (int)(sizeof vd / sizeof vd[0])
#define NF (int)(sizeof vf / sizeof vf[0])
static ll idl(ll x) { return x; }
static int idi(int x) { return x; }
static double idd(double x) { return x; }
struct S { int i; ll l; double d; float f; unsigned char c; };
static struct S mk(int i, ll l, double d) { struct S s; memset(&s, 0, sizeof s); s.i = i; s.l = l; s.d = d; s.f = (float)d; s.c = (unsigned char)i; return s; }
static void dmix(double d) { ull u; memcpy(&u, &d, 8); mix(u); }
int main(void) {
	int i, j, k;
	for (i = 0; i < NL; i++) for (j = 0; j < NL; j++) {
		ll a = vl[i], b = vl[j]; ull ua = a, ub = b;
		mix(a + b); mix(a - b); mix(ua * ub); mix(idl(a) * b); mix(a * idl(b));
		mix(a & b); mix(a | b); mix(a ^ b); mix(~a); mix(-a);
		mix((a < b) | (a <= b) << 1 | (a > b) << 2 | (a >= b) << 3 | (a == b) << 4 | (a != b) << 5);
		mix((ua < ub) | (ua <= ub) << 1 | (ua > ub) << 2 | (ua >= ub) << 3);
		if (b && !(a == (-0x7fffffffffffffffLL - 1) && b == -1)) { mix(a / b); mix(a % b); }
		if (ub) { mix(ua / ub); mix(ua % ub); }
		k = (int)(ub & 63); mix(ua << k); mix(ua >> k); mix(a >> k);
		mix((ull)(int)a); mix((ull)(unsigned)a); mix((ull)(short)a); mix((ull)(signed char)a);
	}
	printf("ll %016llx\n", H); H = 0;
	for (i = 0; i < NI; i++) for (j = 0; j < NL; j++) {
		int x = vi[i]; unsigned ux = x; ll b = vl[j]; ull ub = b;
		mix(x * b); mix(idi(x) * b); mix(b * x); mix(ux * ub); mix(x + b); mix(b - x); mix(ux - ub);
		mix(x < b); mix(ux < b); mix(x == b); mix((ll)x << (j & 31)); mix(b >> (i & 31));
		mix(x * (ll)x); mix(ux * (ull)ux); mix((ll)x * idl(b));
		if (x && !(b == (-0x7fffffffffffffffLL - 1) && x == -1)) { mix(b / x); mix(b % x); }
	}
	printf("mixed %016llx\n", H); H = 0;
	for (i = 0; i < ND; i++) for (j = 0; j < ND; j++) {
		double a = vd[i], b = vd[j];
		dmix(a + b); dmix(a - b); dmix(a * b); dmix(idd(a) * b); dmix(-a);
		if (b != 0) dmix(a / b);
		mix((a < b) | (a <= b) << 1 | (a > b) << 2 | (a >= b) << 3 | (a == b) << 4 | (a != b) << 5);
		dmix((double)(float)a); dmix(a + vf[j % NF]); dmix((float)a * vf[i % NF]);
		if (a > -2e9 && a < 2e9) { mix((ull)(int)a); mix((ull)(ll)a); }
		if (a >= 0 && a < 4e9) mix((unsigned)a);
		if (a > -9e18 && a < 9e18) mix((ull)(ll)a);
	}
	for (i = 0; i < NL; i++) { dmix((double)vl[i]); dmix((double)(ull)vl[i]); dmix((float)vl[i]); dmix((float)(ull)vl[i]); }
	for (i = 0; i < NI; i++) { dmix((double)vi[i]); dmix((double)(unsigned)vi[i]); dmix((float)vi[i]); dmix((float)(unsigned)vi[i]); }
	for (i = 0; i < NF; i++) for (j = 0; j < NF; j++) {
		float a = vf[i], b = vf[j];
		dmix(a + b); dmix(a - b); dmix(a * b); if (b != 0) dmix(a / b);
		mix((a < b) | (a == b) << 1 | (a >= b) << 2);
	}
	printf("fp %016llx\n", H); H = 0;
	for (i = 0; i < NI; i++) {
		struct S s = mk(vi[i], vl[i], vd[i]), t;
		t = s; t.l *= t.i; t.d /= 3; t.f += 1; t.c += 200;
		mix(t.i); mix(t.l); dmix(t.d); dmix(t.f); mix(t.c); mix(s.c * t.c);
		mix(mk(vi[i], vl[i] + 1, 0).l * mk(2, 3, 0).i);
	}
	printf("struct %016llx\n", H);
	return 0;
}
