int main() {
	int a = 3;
	int b = 4;
	int c = 5;
	if (a == 4) {
		return 1;
	} else if (b == 5) {
		return 2;
	} else if (c == 5) {
		return 3; // expect: 3
	} else {
		return 4;
	}
}

