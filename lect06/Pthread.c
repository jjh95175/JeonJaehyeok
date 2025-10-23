#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

int S[100];
int A[100];
int B[100];

void *TaskCode(void *argument)
{
	int tid;
	tid = *((int*) argument);
	for(int i=tid*25; i<(tid+1)*25; i++) {
		S[i] = A[i] + B[i];
	}
	return NULL;
}

int main (int argc, char *argv[])
{
	pthread_t threads[4];
	int args[4];
	int i;

	for(i=0; i<100; i++){
		A[i]=i;
		B[i]=i;
	}

	for (i=0; i<4; ++i) {
		args[i]=i;
		pthread_create(&threads[i], NULL, TaskCode, (void *) &args[i]);
	}

	for (i=0; i<4; ++i)
		pthread_join(threads[i], NULL);

	for(i=0; i<100; i++)
		printf("%d\n",S[i]);
	return 0;
}
