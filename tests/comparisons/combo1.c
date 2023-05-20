int main() {
	int a = 3;
	int b = 4;
	int c = 5;
	if ((a == 3 && b == 4) || c == 1) {
		return 1; // expect: 1
	} else {
		return 0;
	}
}

