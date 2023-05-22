int main() {
	int a = 3;
	int *b = &a;
	*b += 1;
	return a; // expect: 4
}

