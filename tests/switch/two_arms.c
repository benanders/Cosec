int main() {
	int a = 3;
	switch (a) {
		case 3: return 1; // expect: 1
		case 4: return 2;
	}
	return 0;
}
