int main() {
	int sum = 0;
	int i = 1;
	while (i < 5) {
		int j = 1;
		while (j < 5) {
			sum += i * j;
			j += 1;
		}
		i += 1;
	}
	return sum; // expect: 100
}

