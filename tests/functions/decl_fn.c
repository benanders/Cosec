int test();

int test2() {
	return test();
}

int test() {
	return 3;
}

int main() {
	return 3; // expect: 3
}

