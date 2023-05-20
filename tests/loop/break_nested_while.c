int main() {
	int sum = 0;
	int i = 0;
	while (i < 7) {
		int j = 0;
		while (j < 7) {
			if (j >= 2) {
				break;
			}
			sum += i * j;
			j += 1;
		}
		if (sum > 10) {
			break;
		}
		i += 1;
	}
	return sum; // expect: 15
}

