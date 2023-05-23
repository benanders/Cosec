int fn(int x[][3]) {
    return *(*(x + 1) + 1);
}

int main() {
    int a[2][3];
    int *p = a;
    *(p + 4) = 65;
    return fn(a); // expect: 65
}
