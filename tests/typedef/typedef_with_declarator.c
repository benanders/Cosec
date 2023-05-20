int main() {
	typedef int A;
	int b = 3;
	A *a = &b;
	return *a; // expect: 3
}

