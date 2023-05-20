int main() {
	int a = 3;
	int b = 4;
	int c = 5;
	int d = 6;
	if ((a == 1 || b == 4) && c == 5 && d == 6) {
		return 1; // expect: 1
	} else {
		return 0;
	}
}

