int main() {
	int a = 3;
	int b = 4;
	int *c = &a;
	int d = b + *c;
	return d; // expect: 7
}

