int test(int a) {
	a = 3;
	return a;
}
int main() {
	int a = 4;
	test(a);
	return a; // expect: 4
}

