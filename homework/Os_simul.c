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
#define QUANTUM_INIT 1
#define TICK_USEC 100000
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
    int quantum;
    state_t state;
    int io_remaining;
    long wait_ticks;
    long run_ticks;
    long start_tick;
    long end_tick;
} pcb_t;

static pcb_t pcb[NPROC];
static int current_index = -1;
static volatile sig_atomic_t io_request_flag[NPROC];
static volatile sig_atomic_t child_done_flag = 0;
static volatile sig_atomic_t ticks = 0;

static int rand_range(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}

static volatile sig_atomic_t child_burst = 0;
static volatile sig_atomic_t child_running_signal = 0;

void child_sigusr1_handler(int sig) {
    (void)sig;
    child_running_signal = 1;
}

int child_main() {
    srand(getpid() ^ time(NULL));
    child_burst = rand_range(1, 10);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = child_sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) {
        perror("sigaction(SIGUSR1)");
        _exit(1);
    }

    for (;;) {
        pause();

        if (child_running_signal) {
            child_running_signal = 0;

            if (child_burst > 0) {
                child_burst--;

                if (child_burst == 0) {
                    int do_io = rand_range(0, 1); //0=종료, 1=I/O요청
                    if (do_io) {
                        kill(getppid(), SIGUSR2);
                        continue;
                    } else {
                        _exit(0);
                    }
                } else {
                    continue;
                }
            } else {
                _exit(0);
            }
        }
    }
    return 0;
}

void mark_all_ready_quantum_if_exhausted() {
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
    for (int step = 1; step <= NPROC; step++) {
        int j = (start_from + step) % NPROC;
        if (pcb[j].state == STATE_READY && pcb[j].quantum > 0) {
            return j;
        }
    }
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

void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
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

void sigusr2_handler(int sig) {
    (void)sig;
    for (int i = 0; i < NPROC; i++) {
        if (pcb[i].state == STATE_RUNNING) {
            io_request_flag[i] = 1;
            break;
        }
    }
}

void sigalrm_handler(int sig) {
    (void)sig;
    ticks++;

    for (int i = 0; i < NPROC; i++) {
        if (pcb[i].state == STATE_SLEEP) {
            if (pcb[i].io_remaining > 0) {
                pcb[i].io_remaining--;
                if (pcb[i].io_remaining == 0) {
                    pcb[i].state = STATE_READY;
                    if (pcb[i].quantum == 0) pcb[i].quantum = QUANTUM_INIT;
                }
            }
        }
    }

    for (int i = 0; i < NPROC; i++) {
        if (pcb[i].state == STATE_READY) {
            pcb[i].wait_ticks++;
        }
    }

    if (current_index >= 0 && pcb[current_index].state == STATE_RUNNING) {
        if (pcb[current_index].quantum > 0) {
            pcb[current_index].quantum--;
        }

	pcb[current_index].run_ticks++;
        kill(pcb[current_index].pid, SIGUSR1);

        if (io_request_flag[current_index]) {
            io_request_flag[current_index] = 0;
            pcb[current_index].state = STATE_SLEEP;
            pcb[current_index].io_remaining = rand_range(IO_WAIT_MIN, IO_WAIT_MAX);
            current_index = -1;
        } else {
            if (pcb[current_index].quantum == 0) {
                pcb[current_index].state = STATE_READY;
                current_index = -1;
            }
        }
    }
}

void setup_signals_parent() {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction(SIGCHLD)");
        exit(1);
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr2_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGUSR2, &sa, NULL) < 0) {
        perror("sigaction(SIGUSR2)");
        exit(1);
    }

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
    srand(getpid() ^ time(NULL));

    memset(pcb, 0, sizeof(pcb));
    memset((void*)io_request_flag, 0, sizeof(io_request_flag));

    for (int i = 0; i < NPROC; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        } else if (pid == 0) {
            return child_main();
        } else {
            pcb[i].pid = pid;
            pcb[i].quantum = QUANTUM_INIT;
            pcb[i].state = STATE_READY;
            pcb[i].io_remaining = 0;
            pcb[i].wait_ticks = 0;
	    pcb[i].run_ticks = 0;
	    pcb[i].start_tick = ticks;
	    pcb[i].end_tick = 0;
        }
    }

    setup_signals_parent();
    start_timer();

    while (!all_done()) {
        if (current_index < 0) {
            mark_all_ready_quantum_if_exhausted();
            int next = next_ready_index((current_index >= 0) ? current_index : (rand_range(0, NPROC - 1)));
            if (next >= 0) {
                pcb[next].state = STATE_RUNNING;
                current_index = next;
            }
        }

        usleep(1000); //busy-wait을 피하기위해 짧게 휴식

        if (current_index >= 0 && pcb[current_index].state == STATE_DONE) {
            current_index = -1;
        }

    }

    struct itimerval it = {0};
    setitimer(ITIMER_REAL, &it, NULL);

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
