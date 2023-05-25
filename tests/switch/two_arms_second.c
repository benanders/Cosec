int main() {
	int a = 4;
	switch (a) {
		case 3: return 1;
		case 4: return 2; // expect: 2
	}
	return 0;
}
