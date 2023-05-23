int main() {
    int a[2][3];
    int *p = a + 1;
    *p = 1;
    int *q = a;
    *p = 32;
    return *(q + 3); // expect: 32
}
