// index.c — Staging area implementation

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; 
            if (strstr(ent->d_name, ".o") != NULL) continue; 

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { 
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Add object_write declaration to prevent compiler warnings
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // If file doesn't exist, it's just an empty staging area

    uint32_t mode;
    char hex[100];
    uint64_t mtime;
    uint32_t size;
    char path[512];

    while (fscanf(f, "%o %64s %lu %u %511s", &mode, hex, &mtime, &size, path) == 5) {
        if (index->count >= MAX_INDEX_ENTRIES) break;
        IndexEntry *entry = &index->entries[index->count++];
        entry->mode = mode;
        hex_to_hash(hex, &entry->hash);
        entry->mtime_sec = mtime;
        entry->size = size;
        strcpy(entry->path, path);
    }
    fclose(f);
    return 0;
}

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    // 1. Allocate memory on the HEAP instead of the STACK to prevent overflows
    IndexEntry *sorted_entries = NULL;
    
    if (index->count > 0) {
        sorted_entries = malloc(index->count * sizeof(IndexEntry));
        if (!sorted_entries) return -1;
        
        // Copy the active entries and sort them
        memcpy(sorted_entries, index->entries, index->count * sizeof(IndexEntry));
        qsort(sorted_entries, index->count, sizeof(IndexEntry), compare_index_entries);
    }

    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(temp_path, "w");
    if (!f) {
        if (sorted_entries) free(sorted_entries);
        return -1;
    }

    // Write the sorted entries to the file
    for (int i = 0; i < index->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted_entries[i].hash, hex);
        fprintf(f, "%06o %s %lu %u %s\n",
                sorted_entries[i].mode, hex,
                sorted_entries[i].mtime_sec,
                sorted_entries[i].size,
                sorted_entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(temp_path, INDEX_FILE) != 0) {
        unlink(temp_path);
        if (sorted_entries) free(sorted_entries);
        return -1;
    }
    
    if (sorted_entries) free(sorted_entries);
    return 0;
}

int index_add(Index *index, const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "error: could not stat '%s'\n", path);
        return -1;
    }

    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    
    char *buffer = NULL;
    if (st.st_size > 0) {
        buffer = malloc(st.st_size);
        if (!buffer) { fclose(f); return -1; }
        if (fread(buffer, 1, st.st_size, f) != (size_t)st.st_size) {
            free(buffer); fclose(f); return -1;
        }
    }
    fclose(f);

    // Save the file content to the object store as a BLOB
    ObjectID hash;
    if (object_write(OBJ_BLOB, buffer, st.st_size, &hash) != 0) {
        free(buffer); return -1;
    }
    free(buffer);

    // Update or add the entry to the index
    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
        strcpy(entry->path, path);
    }

    entry->mode = mode;
    entry->hash = hash;
    entry->mtime_sec = st.st_mtime;
    entry->size = st.st_size;

    return index_save(index);
}
