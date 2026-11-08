#include <stdio.h>
#include <inttypes.h>
typedef long long ll; typedef unsigned long long ull;
static ll mul(ll a, ll b) { return a * b; }
static ll divi(ll a, ll b) { return a / b; }
static ull udiv(ull a, ull b) { return a / b; }
static ll many(int a, ll b, int c, ll d, int e, ll f, ll g) { return a + b + c + d + e + f + g; }
static ull fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
int main(void) {
	ll a = 1234567890123LL, b = -987654321LL;
	printf("add %lld sub %lld\n", a + b, a - b);
	printf("mul %lld div %lld mod %lld\n", mul(a, 3), divi(a, b), a % b);
	printf("udiv %llu shr %llx shl %llx\n", udiv(~0ULL, 7), (ull)a >> 13, (ull)a << 21);
	printf("sar %lld cmp %d %d %d\n", b >> 9, a > b, (ull)b > (ull)a, b < -1);
	printf("many %lld\n", many(1, 2, 3, 4, 5, 6, 7) + many(1, a, 2, b, 3, a, b));
	printf("fact20 %llu\n", fact(20));
	printf("int64 %" PRId64 " %" PRIx64 "\n", INT64_MIN, UINT64_MAX);
	return 0;
}
