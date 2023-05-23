int main() {
	int a[4];
	a[0] = 1;
	int (*b[3])[4];
	b[0] = &a;
	return (*b[0])[0]; // expect: 1
}

