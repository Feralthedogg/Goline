// src/linux/main.c

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <locale.h>
#include <wchar.h>
#include <strings.h>

#include <immintrin.h>
#include <emmintrin.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#pragma warning(disable:4996)
#endif

wchar_t* to_wide(const char *str) {
    if (!str)
        return NULL;
    size_t len = mbstowcs(NULL, str, 0);
    if (len == (size_t)-1)
        return NULL;
    wchar_t *wstr = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
    if (!wstr)
        return NULL;
    mbstowcs(wstr, str, len + 1);
    return wstr;
}

typedef struct {
    char  *path;
    long   line_count;
} GoFile;

typedef struct {
    GoFile *data;
    size_t size;
    size_t capacity;
} GoFileList;

static void init_go_file_list(GoFileList *list) {
    list->data = NULL;
    list->size = 0;
    list->capacity = 0;
}

static void free_go_file_list(GoFileList *list) {
    for (size_t i = 0; i < list->size; i++) {
        free(list->data[i].path);
    }
    free(list->data);
    list->data = NULL;
    list->size = 0;
    list->capacity = 0;
}

static size_t fast_strlen(const char *str) {
    #if defined(__AVX2__)
        size_t len = 0;
        int mask;
        __asm__ volatile (
            "vxor %%ymm1, %%ymm1, %%ymm1\n\t"       // Zero ymm1
            "1:\n\t"
            "vmovdqu (%[str], %[len], 1), %%ymm0\n\t" // Load 32 bytes from str+len
            "vpcmpeqb %%ymm1, %%ymm0, %%ymm0\n\t"     // Compare with zero
            "vpmovmskb %%ymm0, %%eax\n\t"             // Create mask from comparison
            "test %%eax, %%eax\n\t"                  // Test if any zero byte exists
            "jne 2f\n\t"                             // Jump if found
            "add $32, %[len]\n\t"                    // Increment len by 32
            "jmp 1b\n\t"                             // Loop
            "2:\n\t"
            : [len] "+r" (len), "=a" (mask)
            : [str] "r" (str)
            : "ymm0", "ymm1", "memory"
        );
        int offset = __builtin_ctz(mask);
        return len + offset;
    #elif defined(__SSE2__)
        size_t len = 0;
        int mask;
        __asm__ volatile (
            "pxor %%xmm1, %%xmm1\n\t"                // Zero xmm1
            "1:\n\t"
            "movdqu (%[str], %[len], 1), %%xmm0\n\t"  // Load 16 bytes from str+len
            "pcmpeqb %%xmm1, %%xmm0\n\t"             // Compare with zero
            "pmovmskb %%xmm0, %%eax\n\t"             // Create mask from comparison
            "test %%eax, %%eax\n\t"                 // Test if any zero byte exists
            "jne 2f\n\t"                            // Jump if found
            "add $16, %[len]\n\t"                   // Increment len by 16
            "jmp 1b\n\t"                            // Loop
            "2:\n\t"
            : [len] "+r" (len), "=a" (mask)
            : [str] "r" (str)
            : "xmm0", "xmm1", "memory"
        );
        int offset = __builtin_ctz(mask);
        return len + offset;
    #else
        size_t l = 0;
        while (str[l]) l++;
        return l;
    #endif
}
    
static inline void fast_strcpy(char *dest, const char *src, size_t dest_size) {
    if (dest_size == 0)
        return;
    
    size_t max_copy = dest_size - 1;
    size_t offset = 0;
    #if defined(__AVX2__)
        while (offset + 32 <= max_copy) {
            unsigned int local_mask;
            __asm__ volatile (
                "vxor   %%ymm1, %%ymm1, %%ymm1\n\t"             // Zero ymm1
                "vmovdqu (%3, %1, 1), %%ymm0\n\t"                // Load 32 bytes from src+offset
                "vpcmpeqb %%ymm1, %%ymm0, %%ymm2\n\t"             // Compare with zero
                "vpmovmskb %%ymm2, %%eax\n\t"                     // Create mask from comparison
                "vmovdqu %%ymm0, (%2, %1, 1)\n\t"                // Store 32 bytes to dest+offset
                "movl   %%eax, %0\n\t"                           // Save mask to local_mask
                : "=r" (local_mask)
                : "r" (offset), "r" (dest), "r" (src)
                : "cc", "eax", "ymm0", "ymm1", "ymm2", "memory"
            );
            if (local_mask != 0) {
                unsigned int pos;
                __asm__ volatile (
                    "bsfl %1, %0" // Get index of least significant set bit
                    : "=r" (pos)
                    : "r" (local_mask)
                );
                offset += pos;
                dest[offset] = '\0';
                return;
            }
            offset += 32;
        }
    #elif defined(__SSE2__)
        while (offset + 16 <= max_copy) {
            unsigned int local_mask;
            __asm__ volatile (
                "pxor   %%xmm1, %%xmm1\n\t"                     // Zero xmm1
                "movdqu (%3, %1, 1), %%xmm0\n\t"                // Load 16 bytes from src+offset
                "movdqa %%xmm0, %%xmm2\n\t"                     // Copy xmm0 to xmm2
                "pcmpeqb %%xmm1, %%xmm2\n\t"                    // Compare with zero
                "pmovmskb %%xmm2, %%eax\n\t"                    // Create mask from comparison
                "movdqu %%xmm0, (%2, %1, 1)\n\t"                // Store 16 bytes to dest+offset
                "movl   %%eax, %0\n\t"                          // Save mask to local_mask
                : "=r" (local_mask)
                : "r" (offset), "r" (dest), "r" (src)
                : "cc", "eax", "xmm0", "xmm1", "xmm2", "memory"
            );
            if (local_mask != 0) {
                unsigned int pos;
                __asm__ volatile (
                    "bsfl %1, %0" // Get index of least significant set bit
                    : "=r" (pos)
                    : "r" (local_mask)
                );
                offset += pos;
                dest[offset] = '\0';
                return;
            }
            offset += 16;
        }
    #endif

    while (offset < max_copy && src[offset] != '\0') {
        dest[offset] = src[offset];
        offset++;
    }
    dest[offset] = '\0';
}
    
static void push_go_file(GoFileList *list, const char *path) {
    if (list->size == list->capacity) {
        size_t new_cap = (list->capacity == 0) ? 64 : list->capacity * 2;
        GoFile *new_data = (GoFile *)realloc(list->data, new_cap * sizeof(GoFile));
        if (!new_data) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(1);
        }
        list->data = new_data;
        list->capacity = new_cap;
    }
    size_t len = fast_strlen(path);
    list->data[list->size].path = (char*)malloc(len + 1);
    fast_strcpy(list->data[list->size].path, path, len + 1);
    list->data[list->size].line_count = 0;
    list->size++;
}
    
static long count_non_empty_lines(const char *str, long length) {
    long count = 0;
    int in_line = 0;
    long i = 0;
    #if defined(__AVX2__)
        for (; i <= length - 32; i += 32) {
            int nl_mask;
            __asm__ volatile (
                "vmovdqu (%[str], %[i], 1), %%ymm0\n\t"
                "vpcmpeqb $0x0A, %%ymm0, %%ymm1\n\t"  // Compare with newline (0x0A)
                "vpmovmskb %%ymm1, %%eax\n\t"          // Create mask from comparison
                : "=a" (nl_mask)
                : [str] "r" (str), [i] "r" (i)
                : "ymm0", "ymm1", "memory"
            );
            for (int j = 0; j < 32; j++) {
                char c = str[i+j];
                if (c == '\n') {
                    if (in_line)
                        count++;
                    in_line = 0;
                } else if (c != ' ' && c != '\t' && c != '\r') {
                    in_line = 1;
                }
            }
        }
    #endif
    for (; i < length; i++) {
        char c = str[i];
        if (c == '\n') {
            if (in_line)
                count++;
            in_line = 0;
        } else if (c != ' ' && c != '\t' && c != '\r') {
            in_line = 1;
        }
    }
    if (in_line)
        count++;
    return count;
}
    
long remove_comments(const char *input, char *output, long size, long capacity) {
    int state = 0;
    long out_len = 0;
    long i = 0;
    #if defined(__AVX2__)
        while (i <= size - 32 && out_len <= capacity - 32 && state == 0) {
            int mask;
            __asm__ volatile (
                "vmovdqu (%[input], %[i], 1), %%ymm0\n\t"
                "vpcmpeqb %%ymm_slash, %%ymm0, %%ymm1\n\t"  // Compare with '/'
                "vpcmpeqb %%ymm_dq, %%ymm0, %%ymm2\n\t"     // Compare with '\"'
                "vpcmpeqb %%ymm_bt, %%ymm0, %%ymm3\n\t"     // Compare with '`'
                "vpcmpeqb %%ymm_sq, %%ymm0, %%ymm4\n\t"     // Compare with '\''
                "vorps %%ymm1, %%ymm2, %%ymm1\n\t"
                "vorps %%ymm3, %%ymm4, %%ymm3\n\t"
                "vorps %%ymm1, %%ymm3, %%ymm1\n\t"
                "vpmovmskb %%ymm1, %%eax\n\t"
                : "=a" (mask)
                : [input] "r" (input), [i] "r" (i),
                  [ymm_slash] "x" (_mm256_set1_epi8('/')),
                  [ymm_dq] "x" (_mm256_set1_epi8('"')),
                  [ymm_bt] "x" (_mm256_set1_epi8('`')),
                  [ymm_sq] "x" (_mm256_set1_epi8('\''))
                : "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "memory"
            );
            if (mask == 0) {
                __asm__ volatile (
                    "vmovdqu (%[input], %[i], 1), %%ymm0\n\t"
                    "vmovdqu %%ymm0, (%[output], %[out_len], 1)\n\t"
                    :
                    : [input] "r" (input), [output] "r" (output),
                      [i] "r" (i), [out_len] "r" (out_len)
                    : "ymm0", "memory"
                );
                i += 32;
                out_len += 32;
            } else {
                break;
            }
        }
    #endif
    for (; i < size && out_len < capacity - 1; ) {
        unsigned char c = (unsigned char)input[i++];
        switch (state) {
            case 0:
                if (c == '/') {
                    if (i < size) {
                        unsigned char c2 = (unsigned char)input[i];
                        if (c2 == '/') {
                            state = 1;
                            i++;
                            continue;
                        } else if (c2 == '*') {
                            state = 2;
                            i++;
                            continue;
                        }
                    }
                    output[out_len++] = c;
                } else if (c == '"') {
                    state = 3;
                    output[out_len++] = c;
                } else if (c == '`') {
                    state = 4;
                    output[out_len++] = c;
                } else if (c == '\'') {
                    state = 5;
                    output[out_len++] = c;
                } else {
                    output[out_len++] = c;
                }
                break;
            case 1:
                if (c == '\n') {
                    output[out_len++] = c;
                    state = 0;
                }
                break;
            case 2:
                if (c == '\n') {
                    output[out_len++] = c;
                } else if (c == '*') {
                    if (i < size && input[i] == '/') {
                        i++;
                        state = 0;
                    }
                }
                break;
            case 3:
                if (c == '\\' && i < size) {
                    output[out_len++] = c;
                    c = (unsigned char)input[i++];
                    if (out_len < capacity - 1)
                        output[out_len++] = c;
                } else {
                    if (c == '"')
                        state = 0;
                    output[out_len++] = c;
                }
                break;
            case 4:
                if (c == '`')
                    state = 0;
                output[out_len++] = c;
                break;
            case 5:
                if (c == '\\' && i < size) {
                    output[out_len++] = c;
                    c = (unsigned char)input[i++];
                    if (out_len < capacity - 1)
                        output[out_len++] = c;
                } else {
                    if (c == '\'')
                        state = 0;
                    output[out_len++] = c;
                }
                break;
        }
    }
    if (out_len < capacity)
        output[out_len] = '\0';
    return out_len;
}
    
static int process_one_file(const char *path, long *pLineCount) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open file: '%s': %s\n", path, strerror(errno));
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *input = (char*)malloc(sz + 1);
    if (!input) {
        fclose(fp);
        fprintf(stderr, "Memory allocation failed (input buffer)\n");
        return -1;
    }

    size_t read_bytes = fread(input, 1, sz, fp);
    if (read_bytes != (size_t)sz) {
        fprintf(stderr, "Failed to read entire file: '%s' (%zu / %ld bytes read)\n", path, read_bytes, sz);
        fclose(fp);
        free(input);
        return -1;
    }

    fclose(fp);
    input[sz] = '\0';

    char *output = (char*)malloc(sz + 1);
    if (!output) {
        free(input);
        fprintf(stderr, "Memory allocation failed (output buffer)\n");
        return -1;
    }

    long out_len = remove_comments(input, output, sz, sz + 1);
    if (out_len < 0)
        out_len = 0;
    if (out_len < sz + 1)
        output[out_len] = '\0';

    long lines = count_non_empty_lines(output, out_len);
    *pLineCount = lines;

    free(input);
    free(output);
    return 0;
}

    
static void find_go_files(const char *root, GoFileList *list) {
    DIR *dir = opendir(root);
    if (!dir)
        return;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char fullpath[PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", root, entry->d_name);
        
        struct stat st;
        if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            find_go_files(fullpath, list);
        } else {
            size_t len = fast_strlen(entry->d_name);
            if (len > 3 && strcasecmp(entry->d_name + (len - 3), ".go") == 0) {
                push_go_file(list, fullpath);
            }
        }
    }
    closedir(dir);
}
    
static long compute_dir_go_lines(const char *dir, const GoFileList *list) {
    long sum = 0;
    size_t dlen = fast_strlen(dir);
    for (size_t i = 0; i < list->size; i++) {
        const char *p = list->data[i].path;
        if (strncasecmp(p, dir, dlen) == 0) {
            char c = p[dlen];
            if (c == '\0' || c == '/')
                sum += list->data[i].line_count;
        }
    }
    return sum;
}
    
static int has_go_file_in_dir(const char *dir, const GoFileList *list) {
    long lines = compute_dir_go_lines(dir, list);
    return (lines > 0);
}
    
static void print_progress_bar_with_filename(size_t current, size_t total, const char *filepath) {
    int barWidth = 50;
    double progress = (double)current / (double)total;
    int pos = (int)(barWidth * progress);
    
    const char *filename = strrchr(filepath, '/');
    if (filename)
        filename++;
    else
        filename = filepath;
    
    printf("\r%-80s\r", "");
    
    printf("[");
    for (int i = 0; i < barWidth; i++) {
        if (i < pos)
            printf("=");
        else if (i == pos)
            printf(">");
        else
            printf(" ");
    }
    printf("] %3d%%  (%s)", (int)(progress * 100), filename);
    fflush(stdout);
}
    
typedef struct {
    char name[PATH_MAX];
    int is_dir;
} DirEntry;
    
static void print_tree_only_go(const char *dir, const char *prefix, int is_last, const GoFileList *list) {
    long sum = compute_dir_go_lines(dir, list);
    if (sum == 0)
        return;

    const char *slash = strrchr(dir, '/');
    const char *basename = slash ? (slash + 1) : dir;

    if (prefix[0] == '\0') {
        printf("%s  %ld lines\n", basename, sum);
    } else {
        printf("%s%s%s  %ld lines\n",
               prefix,
               (is_last ? "└── " : "├── "),
               basename,
               sum);
    }

    char newPrefix[256];
    snprintf(newPrefix, sizeof(newPrefix), "%s%s", prefix, (is_last ? "    " : "│   "));

    DIR *dp = opendir(dir);
    if (!dp)
        return;

    struct dirent *entry;
    DirEntry items[1000];
    int count = 0;
    while ((entry = readdir(dp)) != NULL && count < 1000) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        items[count].is_dir = 0;
        fast_strcpy(items[count].name, entry->d_name, sizeof(items[count].name));

        char full[PATH_MAX];
        int written = snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
        if (written < 0 || written >= sizeof(full)) {
            fprintf(stderr, "Path too long: '%s/%s'\n", dir, entry->d_name);
            continue;
        }

        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            items[count].is_dir = 1;
        count++;
    }
    closedir(dp);

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcasecmp(items[i].name, items[j].name) > 0) {
                DirEntry tmp = items[i];
                items[i] = items[j];
                items[j] = tmp;
            }
        }
    }

    int realCount = 0;
    for (int i = 0; i < count; i++) {
        char full[PATH_MAX];
        int written = snprintf(full, sizeof(full), "%s/%s", dir, items[i].name);
        if (written < 0 || written >= sizeof(full)) {
            fprintf(stderr, "Path too long: '%s/%s'\n", dir, items[i].name);
            continue;
        }

        if (items[i].is_dir) {
            if (has_go_file_in_dir(full, list))
                realCount++;
        } else {
            size_t ln = fast_strlen(items[i].name);
            if (ln > 3 && strcasecmp(items[i].name + (ln - 3), ".go") == 0)
                realCount++;
        }
    }

    int passIndex = 0;
    for (int i = 0; i < count; i++) {
        char full[PATH_MAX];
        int written = snprintf(full, sizeof(full), "%s/%s", dir, items[i].name);
        if (written < 0 || written >= sizeof(full)) {
            fprintf(stderr, "Path too long: '%s/%s'\n", dir, items[i].name);
            continue;
        }

        int lastChild = 0;
        if (items[i].is_dir) {
            if (!has_go_file_in_dir(full, list))
                continue;
            passIndex++;
            lastChild = (passIndex == realCount);
            print_tree_only_go(full, newPrefix, lastChild, list);
        } else {
            size_t ln = fast_strlen(items[i].name);
            if (!(ln > 3 && strcasecmp(items[i].name + (ln - 3), ".go") == 0))
                continue;
            passIndex++;
            lastChild = (passIndex == realCount);
            long lines = 0;
            for (size_t k = 0; k < list->size; k++) {
                if (strcasecmp(list->data[k].path, full) == 0) {
                    lines = list->data[k].line_count;
                    break;
                }
            }
            printf("%s%s%s  %ld lines\n",
                   newPrefix,
                   (lastChild ? "└── " : "├── "),
                   items[i].name,
                   lines);
        }
    }
}
    
int main(int argc, char** argv) {
    setlocale(LC_ALL, "");

    const char *root_dir = (argc > 1) ? argv[1] : ".";
    char fullRoot[PATH_MAX];
    if (realpath(root_dir, fullRoot) == NULL) {
        fprintf(stderr, "Failed to resolve path: '%s': %s\n", root_dir, strerror(errno));
        return 1;
    }

    GoFileList g;
    init_go_file_list(&g);
    find_go_files(fullRoot, &g);
    if (g.size == 0) {
        printf("No .go files found under: %s\n", fullRoot);
        free_go_file_list(&g);
        return 0;
    }

    printf("Loading .go files...\n");
    for (size_t i = 0; i < g.size; i++) {
        process_one_file(g.data[i].path, &g.data[i].line_count);
        print_progress_bar_with_filename(i + 1, g.size, g.data[i].path);
        usleep(500000);
    }
    printf("\nDone.\n");

    if (system("clear") != 0) {
        fprintf(stderr, "Failed to clear the screen.\n");
    }

    printf("Total .go files: %zu\n\n", g.size);
    print_tree_only_go(fullRoot, "", 1, &g);

    free_go_file_list(&g);
    return 0;
}

