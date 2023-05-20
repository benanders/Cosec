int main() {
	int a = 3;
	int b = 4;
	int c = 5;
	int d = 6;
	if ((a == 3 && b == 4) || c == 10 || d == 11) {
		return 1; // expect: 1
	} else {
		return 0;
	}
}

