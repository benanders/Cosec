int main() {
	int a[3];
	*(a + 1) = 1;
	return a[1]; // expect: 1
}

