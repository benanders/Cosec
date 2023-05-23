int main() {
	int a[3][4];
	a[0][0] = 1;
	a[2][1] = 4;
	return a[0][0] + a[2][1]; // expect: 5
}

