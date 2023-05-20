int main() {
	typedef int *A;
	int a = 3;
	A b = &a;
	A *c = &b;
	return **c; // expect: 3
}

