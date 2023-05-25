int main() {
	int z = 0;
	switch (7) {
		if (1)
			case 5: z += 2;
		if (0)
			case 7: z += 3;
		if (1)
			case 6: z += 5;
	}
	return z; // expect: 8
}
