int main() {
	int a = 3;
	int *b = &a;
	*b = 4;
	return a; // expect: 4
}

