int main() {
	int j = 0;
	for (int i = 0; i < 100; i += 1) {
		if (i >= 50) {
			j = i;
			break;
		}
	}
	return j; // expect: 50
}

