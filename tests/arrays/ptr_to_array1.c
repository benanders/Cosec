int main() {
	int a[3];
	int (*b)[3] = &a;
	(*b)[0] = 1;
	return a[0]; // expect: 1
}

