int test(int a, int b) {
	return a + b;
}
int main() {
	int a = 3;
	int b = 4;
	int c = test(a, b);
	return c; // expect: 7
}

