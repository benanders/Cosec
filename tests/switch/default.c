int main() {
	int a = 10;
	switch (a) {
		case 3:
			a = 11;
			break;
		case 4:
			a = 20;
			break;
		default:
			a = 12;
			break;
	}
	return a; // expect: 12
}
