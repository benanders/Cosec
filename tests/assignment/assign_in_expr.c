// expect: 4
int main() {
	int a = 3;
	int b = (a = 4, 5);
	return a;
}
