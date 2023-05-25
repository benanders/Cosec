int main() {
	int a = 3;
	switch (a) {
		case 3:
			a = 4;
			break;
	}
	return a; // expect: 4
}
