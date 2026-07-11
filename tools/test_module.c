/*
 * Test WASM module for wasp. No imports, i32-only signatures (the v1
 * calling convention). Build with tools/build_test_module.sh.
 */

__attribute__((export_name("add"))) int add(int a, int b)
{
	return a + b;
}

__attribute__((export_name("fib"))) int fib(int n)
{
	int a = 0, b = 1;

	while (n-- > 0) {
		int t = a + b;

		a = b;
		b = t;
	}
	return a;
}

/* Deliberately traps (unreachable) — exercises the ERROR(TRAP) path. */
__attribute__((export_name("boom"))) int boom(void)
{
	__builtin_trap();
}
