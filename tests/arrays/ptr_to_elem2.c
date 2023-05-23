int main() {
	int a[3];
	a[0] = 3;
	a[1] = 1;
	int *b = &a[0];
	b++;
	return *b; // expect: 1
}

