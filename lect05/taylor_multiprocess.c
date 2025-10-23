#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#define _USE_MATH_DEFINES
#include <math.h>
#define N 4
#define MAXLINE 100

void sinx_taylor(int num_elements, int terms, double* x, double* result)
{
       	int fd[2 * N]; // 각 x마다 파이프 1쌍
	pid_t pid;
    	char message[MAXLINE], line[MAXLINE];

   	for (int i = 0; i < num_elements; i++) {
        	// 파이프 생성
        	if (pipe(fd + 2 * i) < 0) {
        	perror("pipe error");
        	exit(1);
       		}

       		pid = fork();
        	if (pid < 0) {
            	perror("fork error");
            	exit(1);
        	}

        	if (pid == 0) { // --- 자식 프로세스 ---
            		close(fd[2 * i]); // 읽기 끝 닫기

           		double value = x[i];
           		double numer = x[i] * x[i] * x[i];
           		double denom = 6.0;
           		int sign = -1;

            		for (int j = 1; j < terms; j++) {
                		value += (double)sign * numer / denom;
                		numer *= x[i] * x[i];
                		denom *= (2.0 * j + 2.0) * (2.0 * j + 3.0);
                		sign *= -1;
            		}

            		// 문자열로 변환하여 부모에게 전송
            		snprintf(message, sizeof(message), "%lf", value);
            		write(fd[2 * i + 1], message, strlen(message) + 1);

            		close(fd[2 * i + 1]);
            		exit(i); // 자식은 자신의 index 반환
        	} else {
            		close(fd[2 * i + 1]); // 부모는 쓰기 끝 닫기
        	}
    	}

    	// --- 부모 프로세스 ---
    	for (int i = 0; i < num_elements; i++) {
        	int status;
        	pid_t wpid = wait(&status);
        	if (WIFEXITED(status)) {
            	int child_id = WEXITSTATUS(status);
            	read(fd[2 * child_id], line, MAXLINE);
            	result[child_id] = atof(line);
            	close(fd[2 * child_id]);
		}
	}
}

int main() {
    double x[N] = {0.0, M_PI / 6, M_PI / 4, M_PI / 2};
    double result[N];

    sinx_taylor(N, 10, x, result);

    printf("sin(x) 계산 결과:\n");
    for (int i = 0; i < N; i++) {
        printf("x = %lf -> sin(x) ≈ %lf (math.h = %lf)\n", x[i], result[i], sin(x[i]));
    }

    return 0;
}
