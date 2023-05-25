int main() {
	int a = 3;
a:
	if (a >= 5) {
		return a; // expect: 5
	}
	a++;
	goto a;
	return 0;
}
