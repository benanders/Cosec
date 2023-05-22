void test() {}
int main() {
	int (*a)() = &test;
	a();
	return 3; // expect: 3
}

