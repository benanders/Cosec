int main() {
	int a = 4;
	switch (a) {
		case 3:
			a = 10;
			break;
		case 4:
			a = 11;
			break;
		case 5:
			a = 12;
			break;
	}
	return a; // expect: 11
}
