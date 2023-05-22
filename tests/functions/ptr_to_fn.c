int test() {}

int main() {
	int (*a)() = &test;
	return 3; // expect: 3
}

