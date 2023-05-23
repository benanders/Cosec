int i = 3;
void test() {
	i++;
}
int main() {
	i++;
	test();
	return i; // expect: 5
}

