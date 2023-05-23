int main() {
	int a[] = {1, 2, [10] = 100, 20};
	return a[11]; // expect: 20
}
