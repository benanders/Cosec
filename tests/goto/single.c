int main() {
	int a = 4;
	goto a;
	a = 5;
a:
	return a; // expect: 4
}
