int main() {
	int i;
	int acc = 0;
	for (i = 0; i < 5; i++) {
		acc = acc + i;
	}
	return acc; // expect: 10
}
