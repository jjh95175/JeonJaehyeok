#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
	int n1;
	int n2;
	int a;

	n1 = atoi(argv[1]);
	n2 = atoi(argv[3]);
	
	printf("%d\n", n1);
	printf("%d\n", n2);
	
	if (*argv[2] == '+')
		a = n1 + n2;
	else if(*argv[2] == 'X')
		a = n1 * n2;

	printf("%d", a);

	exit(0);
}
