int main() {
	int (*b)() = &main;
	b++;
	return 3; // expect: 3
}

