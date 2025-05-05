#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define MAX_PROCESSES   100
#define MAX_FRAMES      1024
#define FRAMES_PER_ROW  4

#define C_RESET    "\x1b[0m"
#define C_INFO     "\x1b[1;34m"
#define C_OK       "\x1b[1;32m"
#define C_WARN     "\x1b[1;33m"
#define C_ERR      "\x1b[1;31m"
#define C_FREE     "\x1b[90m"

typedef struct {
    int pid;
    int arrival_time;
    int size;
    int num_pages;
    int duration;
    int departure_time;
    int allocated_count;
    int allocated_frames[MAX_FRAMES];
} Process;

typedef struct {
    int pid;
    int page_number;
    int next_free;
} Frame;

typedef struct {
    int time;
    int pid;
    int type;
} Event;

int total_memory, page_size, num_frames;
Frame frames[MAX_FRAMES];

int free_list_head = -1;

int queue[MAX_FRAMES], front = 0, rear = 0, qcount = 0;

int total_evictions = 0;
int total_allocs = 0;

// Total internal fragmentation
// This is the total amount of memory wasted in the last page of each process  that is not fully used. For example, if a process needs 5 KB and the page size is 4 KB, then the last page will have 3 KB of internal fragmentation.
int total_internal_frag = 0;
int total_processes_handled = 0;

// I push a frame into the free list
void free_list_push(int f) {
    frames[f].next_free = free_list_head;
    free_list_head = f;
}

// I pop a frame from the free list
int free_list_pop() {
    if (free_list_head == -1) return -1;
    int f = free_list_head;
    free_list_head = frames[f].next_free;
    return f;
}

enum { ADD = 1, REM = 0 };

// I enqueue a frame into FIFO queue
void enqueue(int f) {
    queue[rear] = f;
    rear = (rear + 1) % num_frames;
    qcount++;
}

// dequeue a valid frame (non-free) from FIFO queue
int dequeue_valid() {
    while (qcount > 0) {
        int f = queue[front];
        front = (front + 1) % num_frames;
        qcount--;
        if (frames[f].pid != -1) return f;
    }
    return -1;
}

// Sorting the  processes using quick sort 
int compare_event(const void *a, const void *b) {
    const Event *e1 = a, *e2 = b;
    if (e1->time != e2->time) return e1->time - e2->time;
    return e1->type - e2->type;
}

// This function iss for print a separator line
void print_sep(char c) {
    for (int i = 0; i < 80; i++) putchar(c);
    putchar('\n');
}

// display the current memory frame status
void print_memory(int time) {
    for (int i = 0; i < num_frames; i++) {
        if (frames[i].pid == -1) {
            printf(C_FREE "[%03d]: Free " C_RESET, i);
        } else {
            printf(C_OK "[%03d]:P%d Pg%d " C_RESET,
                   i, frames[i].pid, frames[i].page_number);
        }
        if ((i + 1) % FRAMES_PER_ROW == 0) putchar('\n');
    }
    putchar('\n');
}

// This function is to print a formatted header for each event
void print_event_header(int time, const char *msg, const char *color) {
    print_sep('=');
    printf("%sTIME %3d  %s%s\n", color, time, msg, C_RESET);
    print_sep('=');
}


int main() {
    printf("Enter total memory (KB): ");
    if (scanf("%d", &total_memory) != 1) return 1;
    printf("Enter page size   (KB): ");
    if (scanf("%d", &page_size) != 1) return 1;
    num_frames = total_memory / page_size;
    if (num_frames < 1 || num_frames > MAX_FRAMES) {
        fprintf(stderr, C_ERR "Error: num_frames must be 1..%d\n" C_RESET, MAX_FRAMES);
        return 1;
    }

    for (int i = 0; i < num_frames; i++) {
        frames[i].pid = frames[i].page_number = -1;
        frames[i].next_free = i + 1;
    }
    frames[num_frames - 1].next_free = -1;
    free_list_head = 0;

    Process procs[MAX_PROCESSES];
    int n = 0;
    FILE *fp = fopen("..processes/process.txt", "r");
    if (!fp) { perror("../processes/process.txt"); return 1; }

    while (n < MAX_PROCESSES &&
           fscanf(fp, "%d %d %d",
                  &procs[n].arrival_time,
                  &procs[n].size,
                  &procs[n].duration) == 3) {
        procs[n].pid = n + 1;
        procs[n].num_pages = (procs[n].size + page_size - 1) / page_size;
        if (procs[n].num_pages == 0) procs[n].num_pages = 1;
        procs[n].departure_time = procs[n].arrival_time + procs[n].duration;
        procs[n].allocated_count = 0;

        int used_in_last_page = procs[n].size % page_size;
        if (used_in_last_page == 0) {
            used_in_last_page = page_size;
        }
        total_internal_frag += (page_size - used_in_last_page);

        if (procs[n].num_pages > num_frames) {
            printf(C_WARN "Warning: Process %d needs %d pages, but only %d frames exist. Will use FIFO replacements.\n" C_RESET,
                   procs[n].pid, procs[n].num_pages, num_frames);
        }
        n++;
    }
    fclose(fp);
    if (n == 0) {
        fprintf(stderr, C_ERR "No processes loaded.\n" C_RESET);
        return 1;
    }

    Event events[2 * MAX_PROCESSES];
    int evt = 0;
    for (int i = 0; i < n; i++) {
        events[evt++] = (Event){procs[i].arrival_time, procs[i].pid, ADD};
        events[evt++] = (Event){procs[i].departure_time, procs[i].pid, REM};
    }
    qsort(events, evt, sizeof(Event), compare_event);

    for (int i = 0; i < evt; i++) {
        Event *e = &events[i];
        Process *p = &procs[e->pid - 1];

        if (e->type == REM) {
            print_event_header(e->time, "Process Termination", C_WARN);
            printf(" → Freeing %d pages of P%d\n\n", p->allocated_count, p->pid);

            for (int k = 0; k < p->allocated_count; k++) {
                int f = p->allocated_frames[k];
                if (frames[f].pid == p->pid) {
                    frames[f].pid = frames[f].page_number = -1;
                    free_list_push(f);
                }
            }
            p->allocated_count = 0;
            print_memory(e->time);

        } else {
            print_event_header(e->time, "Process Arrival", C_INFO);
            printf(" → P%d needs %d pages\n\n", p->pid, p->num_pages);

            int allocd = 0;
            while (free_list_head != -1 && allocd < p->num_pages) {
                int f = free_list_pop();
                frames[f].pid = p->pid;
                frames[f].page_number = allocd;
                enqueue(f);
                p->allocated_frames[p->allocated_count++] = f;
                allocd++; total_allocs++;
            }
            while (allocd < p->num_pages) {
                int f = dequeue_valid();
                if (f < 0) break;
                total_evictions++;
                printf(C_ERR "  Evicting frame %03d (P%d Pg%d)\n" C_RESET,
                       f, frames[f].pid, frames[f].page_number);
                frames[f].pid = p->pid;
                frames[f].page_number = allocd;
                enqueue(f);
                p->allocated_frames[p->allocated_count++] = f;
                allocd++;
            }

            printf("\n" C_OK "Process %d fully allocated.%s\n\n", p->pid, C_RESET);
            print_memory(e->time);

            total_processes_handled++;
        }
    }

    print_sep('=');
    printf(" Total allocations: %d\n", total_allocs);
    printf(" Total evictions:   %d\n", total_evictions);
    printf(" Total processes handled: %d\n", total_processes_handled);
    printf(" Total internal fragmentation: %d KB\n", total_internal_frag);
    printf(" Average frames per process: %.2f\n",
           total_processes_handled ? (float)total_allocs / total_processes_handled : 0.0);
    print_sep('=');

    return 0;
}