int main() {
	int i = 0;
	while (i < 100) {
		if (i >= 50) {
			break;
		}
		i += 1;
	}
	return i; // expect: 50
}

