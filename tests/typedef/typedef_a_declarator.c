int main() {
	typedef int *A;
	int a = 3;
	A b = &a;
	return *b; // expect: 3
}

