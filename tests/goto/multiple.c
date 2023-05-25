int main() {
	int a = 3;
a:
	a = 4;
	goto c;
b:
	a = 5;
c:
	return a; // expect: 4
}
