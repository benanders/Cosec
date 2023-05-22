int test(int a) {
	a = 3;
	return a;
}
int main() {
	int a = 4;
	return test(a); // expect: 3
}

