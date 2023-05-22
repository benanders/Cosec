int main() {
	int a = 3;
	int b = 4;
	int *c = &a;
	int *d = &b;
	int e = c - d;
	return e; // expect: 1
}

