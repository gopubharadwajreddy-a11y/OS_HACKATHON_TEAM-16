
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <time.h>

#define PIDFILE "/tmp/processctl.pid"
#define LOGFILE "process_events.log"

static volatile sig_atomic_t running = 1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter = 0;

void log_event(const char *event, pid_t pid)
{
    int fd = open(LOGFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd < 0) {
        perror("open");
        return;
    }

    char buffer[256];

    int len = snprintf(buffer, sizeof(buffer),
                       "%ld | %s | PID=%ld\n",
                       (long)time(NULL), event, (long)pid);

    if (len > 0)
        write(fd, buffer, (size_t)len);

    close(fd);
}

void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

pid_t get_worker_pid(void)
{
    FILE *file = fopen(PIDFILE, "r");

    if (!file)
        return -1;

    long pid;
    int result = fscanf(file, "%ld", &pid);
    fclose(file);

    if (result != 1 || pid <= 1)
        return -1;

    return (pid_t)pid;
}

void start_process(void)
{
    pid_t oldpid = get_worker_pid();

    if (oldpid > 1 && kill(oldpid, 0) == 0) {
        printf("Worker already running: PID %ld\n", (long)oldpid);
        return;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        execl("./processctl", "processctl", "worker", NULL);
        perror("execl");
        _exit(127);
    }

    FILE *file = fopen(PIDFILE, "w");

    if (!file) {
        perror("PID file");
        kill(pid, SIGTERM);
        return;
    }

    fprintf(file, "%ld\n", (long)pid);
    fclose(file);

    printf("Worker started. PID: %ld\n", (long)pid);
    log_event("START", pid);
}

void control_process(const char *command, int sig)
{
    pid_t pid = get_worker_pid();

    if (pid <= 1) {
        printf("No worker PID found. Start a worker first.\n");
        return;
    }

    if (kill(pid, sig) == -1) {
        perror("kill");
        return;
    }

    printf("%s signal sent to PID %ld\n", command, (long)pid);
    log_event(command, pid);

    if (sig == SIGTERM || sig == SIGKILL)
        unlink(PIDFILE);
}

void worker_process(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    printf("Worker running. PID: %ld\n", (long)getpid());
    fflush(stdout);

    unsigned long count = 0;

    while (running) {
        printf("Worker heartbeat: %lu\n", ++count);
        fflush(stdout);

        struct timespec delay = {2, 0};

        while (nanosleep(&delay, &delay) == -1 &&
               errno == EINTR && running) {
        }
    }

    printf("Worker terminating gracefully.\n");
}

void shell_demo(void)
{
    char command[256];

    while (1) {
        printf("processctl$ ");
        fflush(stdout);

        if (!fgets(command, sizeof(command), stdin))
            break;

        command[strcspn(command, "\n")] = '\0';

        if (strcmp(command, "exit") == 0)
            break;

        if (command[0] == '\0')
            continue;

        pid_t pid = fork();

        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", command, (char *)NULL);
            perror("execl");
            _exit(127);
        }

        if (pid < 0) {
            perror("fork");
            continue;
        }

        int status;

        while (waitpid(pid, &status, 0) == -1) {
            if (errno == EINTR)
                continue;

            perror("waitpid");
            break;
        }

        if (WIFEXITED(status))
            printf("Exit status: %d\n", WEXITSTATUS(status));
    }
}

void pipe_demo(void)
{
    int fd[2];

    if (pipe(fd) == -1) {
        perror("pipe");
        return;
    }

    pid_t pid = fork();

    if (pid == -1) {
        perror("fork");
        close(fd[0]);
        close(fd[1]);
        return;
    }

    if (pid == 0) {
        close(fd[0]);

        const char message[] = "Message from child process";

        write(fd[1], message, sizeof(message));
        close(fd[1]);

        _exit(0);
    }

    close(fd[1]);

    char buffer[128];
    ssize_t bytes = read(fd[0], buffer, sizeof(buffer) - 1);

    if (bytes >= 0) {
        buffer[bytes] = '\0';
        printf("IPC message: %s\n", buffer);
    } else {
        perror("read");
    }

    close(fd[0]);
    waitpid(pid, NULL, 0);
}

void memory_demo(void)
{
    int *value = mmap(NULL, sizeof(int),
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS,
                      -1, 0);

    if (value == MAP_FAILED) {
        perror("mmap");
        return;
    }

    *value = 10;

    pid_t pid = fork();

    if (pid == 0) {
        *value = 100;
        printf("Child value: %d\n", *value);
        fflush(stdout);
        _exit(0);
    }

    if (pid < 0) {
        perror("fork");
        munmap(value, sizeof(int));
        return;
    }

    waitpid(pid, NULL, 0);

    printf("Parent sees shared value: %d\n", *value);

    munmap(value, sizeof(int));
}

void file_demo(void)
{
    int fd = open(LOGFILE, O_CREAT | O_RDWR | O_APPEND, 0644);

    if (fd == -1) {
        perror("open");
        return;
    }

    char message[128];

    int len = snprintf(message, sizeof(message),
                       "%ld | FILE_DEMO | PID=%ld\n",
                       (long)time(NULL), (long)getpid());

    if (write(fd, message, (size_t)len) == -1)
        perror("write");

    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek");
        close(fd);
        return;
    }

    char buffer[512];
    ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);

    if (bytes >= 0) {
        buffer[bytes] = '\0';
        printf("%s", buffer);
    } else {
        perror("read");
    }

    close(fd);
}

void *thread_task(void *arg)
{
    (void)arg;

    for (int i = 0; i < 5; i++) {
        pthread_mutex_lock(&lock);

        counter++;

        printf("Thread %lu: counter=%ld\n",
               (unsigned long)pthread_self(), counter);

        pthread_mutex_unlock(&lock);
    }

    return NULL;
}

void concurrency_demo(void)
{
    pthread_t t1, t2;

    if (pthread_create(&t1, NULL, thread_task, NULL) != 0) {
        perror("pthread_create");
        return;
    }

    if (pthread_create(&t2, NULL, thread_task, NULL) != 0) {
        perror("pthread_create");
        pthread_join(t1, NULL);
        return;
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("Final counter: %ld\n", counter);
}

void show_status(void){
    pid_t pid = get_worker_pid();

    if (pid <= 1) {
        printf("No worker registered.\n");
        return;
    }

    if (kill(pid, 0) == 0 || errno == EPERM)
        printf("Worker PID: %ld exists\n", (long)pid);
    else
        printf("Worker is not running.\n");
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
    		printf("Linux Signal-Based Process Controller\n\n");
    		printf("Usage:\n");
    		printf("  ./hackathon start\n");
    		printf("  ./hackathon pause\n");
    		printf("  ./hackathon resume\n");
    		printf("  ./hackathon stop\n");
    		printf("  ./hackathon kill\n");
    		printf("  ./hackathon status\n");
    		printf("  ./hackathon shell\n");
    		printf("  ./hackathon ipc\n");
    		printf("  ./hackathon memory\n");
    		printf("  ./hackathon file\n");
    		printf("  ./hackathon threads\n");
    		return 0;
}

    if (strcmp(argv[1], "worker") == 0) {
        worker_process();
    }
    else if (strcmp(argv[1], "start") == 0) {
        start_process();
    }
    else if (strcmp(argv[1], "pause") == 0) {
        control_process("PAUSE", SIGSTOP);
    }
    else if (strcmp(argv[1], "resume") == 0) {
        control_process("RESUME", SIGCONT);
    }
    else if (strcmp(argv[1], "stop") == 0) {
        control_process("STOP", SIGTERM);
    }
    else if (strcmp(argv[1], "kill") == 0) {
        control_process("KILL", SIGKILL);
    }
    else if (strcmp(argv[1], "status") == 0) {
        show_status();
    }
    else if (strcmp(argv[1], "shell") == 0) {
        shell_demo();
    }
    else if (strcmp(argv[1], "ipc") == 0) {
        pipe_demo();
    }
    else if (strcmp(argv[1], "memory") == 0) {
        memory_demo();
    }
    else if (strcmp(argv[1], "file") == 0) {
        file_demo();
    }
    else if (strcmp(argv[1], "threads") == 0) {
        concurrency_demo();
    }
    else {
        printf("Unknown command: %s\n", argv[1]);
        return 1;
    }

    return 0;
}
