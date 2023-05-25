int main() {
	int a = 3;
	switch (a) {
		case 2:
			a = 10;
		case 3:
			a = 11;
		case 4:
			a = 12;
			break;
		default:
			a = 20;
	}
	return a; // expect: 12
}
