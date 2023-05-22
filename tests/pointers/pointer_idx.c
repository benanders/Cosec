int main() {
	int a = 3;
	int *b = &a;
	int c = b[0];
	return c; // expect: 3
}

