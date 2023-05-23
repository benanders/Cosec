int main() {
	int a[3];
	int *b = &a[0];
	*b = 1;
	return a[0]; // expect: 1
}

