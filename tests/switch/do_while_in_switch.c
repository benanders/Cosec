int main() {
	int a = 0;
	int count = 27;
	switch (count % 8) {
		case 0: do {  a++;
		case 7:       a++;
		case 6:       a++;
		case 5:       a++;
		case 4:       a++;
		case 3:       a++;
		case 2:       a++;
		case 1:       a++;
		} while ((count -= 8) > 0);
	}
	return a; // expect: 27
}
