int main() {
	int sum = 0;
	for (int i = 0; i < 100; i++) {
		if (i < 50) {
			continue;
		}
		sum++;
	}
	return sum; // expect: 50
}

