int main() {
    int a[4][5];
    int *p = a;
    *(*(a + 1) + 2) = 62;
    return *(p + 7); // expect: 62
}
