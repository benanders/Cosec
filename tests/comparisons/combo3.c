int main() {
	int a = 3;
	int b = 4;
	int c = 5;
	if ((a == 1 && b == 1) || c == 5) {
		return 1; // expect: 1
	} else {
		return 0;
	}
}

