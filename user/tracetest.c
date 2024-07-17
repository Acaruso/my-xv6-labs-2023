#include "kernel/types.h"
#include "user/user.h"

int main(void) {
    int res = trace(1);
    printf("res: %d", res);
}
