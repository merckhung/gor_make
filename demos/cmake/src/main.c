#include <stdio.h>
int add(int a, int b);
int sub(int a, int b);
int mul(int a, int b);
int main() {
    printf("add(3,4) = %d\n", add(3, 4));
    printf("sub(10,3) = %d\n", sub(10, 3));
    printf("mul(6,7) = %d\n", mul(6, 7));
    return 0;
}
