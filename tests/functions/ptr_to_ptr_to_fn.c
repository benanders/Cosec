int test() {
	return 3;
}
int main() {
	int (*a)() = &test;
	int (**b)() = &a;
	return (*b)(); // expect: 3
}

