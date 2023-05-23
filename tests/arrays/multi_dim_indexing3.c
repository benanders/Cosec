int main() {
    int a[2][3];
    a[0][1] = 1;
    a[1][1] = 2;
    int *p = a;
    return p[4]; // expect: 2
}
