#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define NPROC 10
#define QUANTUM_INIT 3            // 각 프로세스 기본 타임퀀텀
#define TICK_USEC 100000          // 타이머 틱: 100ms
#define IO_WAIT_MIN 1
#define IO_WAIT_MAX 5

typedef enum {
    STATE_READY = 0,
    STATE_RUNNING,
    STATE_SLEEP,
    STATE_DONE
} state_t;

typedef struct {
    pid_t pid;
    int quantum;              // 남은 타임퀀텀
    state_t state;
    int io_remaining;         // 슬립 상태일 때 남은 I/O 대기시간(틱)
    long wait_ticks;          // ready 큐에서 머문 총 틱
    long run_ticks;	      // 실제 실행시간
    long start_tick;	      // 프로세스가 처음 ready큐로 들어온 시각
    long end_tick;	      // 프로세스가 종료된 시각
} pcb_t;

static pcb_t pcb[NPROC];
static int current_index = -1;      // 현재 실행 중인 인덱스
static volatile sig_atomic_t io_request_flag[NPROC]; // 자식별 I/O 요청 플래그
static volatile sig_atomic_t child_done_flag = 0;    // 자식 종료 발생 플래그
static volatile sig_atomic_t ticks = 0;

// 안전한 랜덤: 부모/자식 각각 시드
static int rand_range(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}

// ------------ 자식 로직 ------------
static volatile sig_atomic_t child_burst = 0;  // 자식 CPU 버스트
static volatile sig_atomic_t child_running_signal = 0;

void child_sigusr1_handler(int sig) {
    // 부모가 실행 틱을 보내면 1 감소
    (void)sig;
    child_running_signal = 1;
}

int child_main() {
    // 자식: 초기 CPU 버스트 설정(1~10), 대기 상태 시작
    srand(getpid() ^ time(NULL));
    child_burst = rand_range(1, 10);

    // SIGUSR1 핸들러 등록
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        perror("sigaction(SIGUSR1)");
        _exit(1);
    }

    // 루프: 부모가 SIGUSR1 보내면 1틱 실행
    for (;;) {
        pause(); // SIGUSR1 대기

        if (child_running_signal) {
            child_running_signal = 0;

            // 실행 1틱
            if (child_burst > 0) {
                child_burst--;

                // 버스트가 0이 되면: 종료 또는 I/O 요청(랜덤)
                if (child_burst == 0) {
                    int do_io = rand_range(0, 1); // 0: 종료, 1: I/O
                    if (do_io) {
                        // 부모에게 I/O 요청
                        kill(getppid(), SIGUSR2);
                        // I/O 요청 후: 다시 부모 스케줄링을 기다림 (sleep 전환은 부모가 처리)
                        continue;
                    } else {
                        // 종료
                        _exit(0);
                    }
                } else {
                    // 아직 버스트 남아있으면 다음 틱을 기다림
                    continue;
                }
            } else {
                // 논리적으로 여기 오지 않음. 안전 종료.
                _exit(0);
            }
        }
    }
    return 0;
}

// ------------ 부모(커널) 로직 ------------

void mark_all_ready_quantum_if_exhausted() {
    // 모든 ready 프로세스의 남은 퀀텀이 0이면, ready 상태인 프로세스들의 퀀텀을 초기화
    int any_ready = 0, any_ready_nonzero = 0;
    for (int i = 0; i < NPROC; i++) {
        if (pcb[i].state == STATE_READY) {
            any_ready = 1;
            if (pcb[i].quantum > 0) any_ready_nonzero = 1;
        }
    }
    if (any_ready && !any_ready_nonzero) {
        for (int i = 0; i < NPROC; i++) {
            if (pcb[i].state == STATE_READY) {
                pcb[i].quantum = QUANTUM_INIT;
            }
        }
    }
}

int next_ready_index(int start_from) {
    // 원형으로 다음 ready를 찾음
    for (int step = 1; step <= NPROC; step++) {
        int j = (start_from + step) % NPROC;
        if (pcb[j].state == STATE_READY && pcb[j].quantum > 0) {
            return j;
        }
    }
    // 퀀텀이 모두 0일 수 있으니, 이 경우 ready 중 아무나 선택 후 퀀텀 리필
    for (int step = 1; step <= NPROC; step++) {
        int j = (start_from + step) % NPROC;
        if (pcb[j].state == STATE_READY) {
            return j;
        }
    }
    return -1;
}

int all_done() {
    for (int i = 0; i < NPROC; i++) {
        if (pcb[i].state != STATE_DONE) return 0;
    }
    return 1;
}

// SIGCHLD: 자식 종료 처리
void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    // 여러 자식이 한 번에 종료할 수 있음
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < NPROC; i++) {
            if (pcb[i].pid == pid) {
                pcb[i].state = STATE_DONE;
                pcb[i].quantum = 0;
                pcb[i].io_remaining = 0;
		pcb[i].end_tick = ticks;
                break;
            }
        }
        child_done_flag = 1;
    }
}

// SIGUSR2: 자식의 I/O 요청 처리 플래그 세팅
void sigusr2_handler(int sig) {
    (void)sig;
    // 어떤 자식이 보냈는지 알기 위해 getpid()는 불가. siginfo를 쓰면 좋지만
    // 간단히 모든 RUNNING 프로세스에 대해 I/O 요청으로 간주.
    for (int i = 0; i < NPROC; i++) {
        if (pcb[i].state == STATE_RUNNING) {
            io_request_flag[i] = 1;
            break;
        }
    }
}

// SIGALRM: 타이머 틱 핸들러
void sigalrm_handler(int sig) {
    (void)sig;
    ticks++;

    // 슬립 큐 I/O 남은 시간 감소 및 복귀 처리
    for (int i = 0; i < NPROC; i++) {
        if (pcb[i].state == STATE_SLEEP) {
            if (pcb[i].io_remaining > 0) {
                pcb[i].io_remaining--;
                if (pcb[i].io_remaining == 0) {
                    pcb[i].state = STATE_READY;
                    if (pcb[i].quantum == 0) pcb[i].quantum = QUANTUM_INIT; // 복귀 시 퀀텀 리필
                }
            }
        }
    }

    // ready 큐 대기시간 증가
    for (int i = 0; i < NPROC; i++) {
        if (pcb[i].state == STATE_READY) {
            pcb[i].wait_ticks++;
        }
    }

    // 현재 실행 중인 프로세스 틱 처리
    if (current_index >= 0 && pcb[current_index].state == STATE_RUNNING) {
        // 타임퀀텀 1 감소
        if (pcb[current_index].quantum > 0) {
            pcb[current_index].quantum--;
        }

        // 자식에게 1틱 실행 시그널
	pcb[current_index].run_ticks++;
        kill(pcb[current_index].pid, SIGUSR1);

        // I/O 요청이 들어왔으면 슬립으로 보냄
        if (io_request_flag[current_index]) {
            io_request_flag[current_index] = 0;
            pcb[current_index].state = STATE_SLEEP;
            pcb[current_index].io_remaining = rand_range(IO_WAIT_MIN, IO_WAIT_MAX);
            // 실행 중이던 것을 중단하고 다음으로 넘어갈 준비
            current_index = -1;
        } else {
            // 퀀텀이 0이면 컨텍스트 스위치
            if (pcb[current_index].quantum == 0) {
                pcb[current_index].state = STATE_READY; // 다시 ready로
                current_index = -1;
            }
        }
    }
}

void setup_signals_parent() {
    struct sigaction sa;

    // SIGCHLD
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction(SIGCHLD)");
        exit(1);
    }

    // SIGUSR2 (I/O 요청)
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr2_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR2, &sa, NULL) < 0) {
        perror("sigaction(SIGUSR2)");
        exit(1);
    }

    // SIGALRM (타이머 틱)
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigalrm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGALRM, &sa, NULL) < 0) {
        perror("sigaction(SIGALRM)");
        exit(1);
    }
}

void start_timer() {
    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = TICK_USEC;
    it.it_value.tv_sec = 0;
    it.it_value.tv_usec = TICK_USEC;
    if (setitimer(ITIMER_REAL, &it, NULL) < 0) {
        perror("setitimer");
        exit(1);
    }
}

int main() {
    // 부모: 커널 초기화
    srand(getpid() ^ time(NULL));

    memset(pcb, 0, sizeof(pcb));
    memset((void*)io_request_flag, 0, sizeof(io_request_flag));

    // 자식 생성
    for (int i = 0; i < NPROC; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        } else if (pid == 0) {
            // 자식
            return child_main();
        } else {
            // 부모
            pcb[i].pid = pid;
            pcb[i].quantum = QUANTUM_INIT;
            pcb[i].state = STATE_READY; // 초기엔 ready로 두고, 스케줄링될 때 run
            pcb[i].io_remaining = 0;
            pcb[i].wait_ticks = 0;
	    pcb[i].run_ticks = 0;
	    pcb[i].start_tick = ticks;
	    pcb[i].end_tick = 0;
        }
    }

    setup_signals_parent();
    start_timer();

    // 메인 스케줄링 루프
    while (!all_done()) {
        // 현재 실행 중인 프로세스가 없으면 선택
        if (current_index < 0) {
            mark_all_ready_quantum_if_exhausted();
            int next = next_ready_index((current_index >= 0) ? current_index : (rand_range(0, NPROC - 1)));
            if (next >= 0) {
                pcb[next].state = STATE_RUNNING;
                current_index = next;
            }
        }

        // 스케줄링 결정과 상태 전이는 시그널 핸들러에서 진행되므로 여기선 휴식
        // busy-wait를 피하려고 짧게 휴식
        usleep(1000);

        // 종료한 자식이 있으면 현재 인덱스 조정(핸들러가 done으로 설정함)
        if (current_index >= 0 && pcb[current_index].state == STATE_DONE) {
            current_index = -1;
        }

        // 만약 현재 프로세스가 RUNNING인데 I/O로 바로 빠졌거나 퀀텀 소진으로 READY가 되면, 다음 선택은 다음 루프에서 처리
    }

    // 타이머 정지
    struct itimerval it = {0};
    setitimer(ITIMER_REAL, &it, NULL);

    // 성능 분석 (대기시간, 실행시간, 반환시간)
    long total_wait = 0, total_run = 0, total_turnaround = 0;
    int count = 0;

    printf("\n=== 결과 ===\n");
    for (int i = 0; i < NPROC; i++) {
	long turnaround = pcb[i].end_tick - pcb[i].start_tick;
        printf("PID %d: state=%s, wait_ticks=%ld, run_ticks=%ld, turnaround=%ld\n",
               pcb[i].pid,
               (pcb[i].state == STATE_DONE) ? "DONE" :
               (pcb[i].state == STATE_READY) ? "READY" :
               (pcb[i].state == STATE_RUNNING) ? "RUNNING" : "SLEEP",
               pcb[i].wait_ticks,
	       pcb[i].run_ticks,
	       turnaround);

        total_wait += pcb[i].wait_ticks;
	total_run += pcb[i].run_ticks;
	total_turnaround += turnaround;
        count++;
    }

    double avg_wait = (count > 0) ? (double)total_wait / count : 0.0;
    double avg_run = (count > 0) ? (double)total_run / count : 0.0;
    double avg_turnaround = (count > 0) ? (double)total_turnaround / count : 0.0;

    printf("평균 대기시간: %.2f 틱\n", avg_wait);
    printf("평균 실행시간: %.2f 틱\n", avg_run);
    printf("평균 반환시간: %.2f 틱\n", avg_turnaround);

    return 0;
}
