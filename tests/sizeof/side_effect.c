// expect: 3
int main() {
	int a = 3;
	int b = sizeof(a++);
	return a;
}
