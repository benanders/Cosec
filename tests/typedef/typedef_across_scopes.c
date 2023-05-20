int main() {
	typedef int A;
	{
		int A = 3;
		A = 4;
	}
	A a = 4;
	return a; // expect: 4
}

