// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index

#include "tree.h"
#include "index.h"  // Added to access the Index struct
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; 

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; 

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; 

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; 

        ptr = null_byte + 1; 

        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; 
        
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED: Recursive Tree Builder ─────────────────────────────────────

// Recursive helper function to build directories
static int write_tree_level(Index *idx, int start, int end, int path_offset, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;
    int i = start;

    while (i < end) {
        // Look at the path ignoring the parent directories we've already parsed
        const char *path = idx->entries[i].path + path_offset;
        char *slash = strchr(path, '/');

        TreeEntry *te = &tree.entries[tree.count++];

        if (!slash) {
            // It's a file in the current directory level
            strcpy(te->name, path);
            te->mode = idx->entries[i].mode;
            te->hash = idx->entries[i].hash;
            i++;
        } else {
            // It's a subdirectory. We need to find all files inside it.
            int dir_len = slash - path;
            strncpy(te->name, path, dir_len);
            te->name[dir_len] = '\0';
            te->mode = MODE_DIR; // 0040000

            int j = i;
            while (j < end) {
                const char *j_path = idx->entries[j].path + path_offset;
                // If it shares the same directory prefix, it belongs in this subtree
                if (strncmp(j_path, te->name, dir_len) != 0 || j_path[dir_len] != '/') {
                    break;
                }
                j++;
            }

            // Recursively build the child tree
            ObjectID sub_tree_id;
            if (write_tree_level(idx, i, j, path_offset + dir_len + 1, &sub_tree_id) != 0) {
                return -1;
            }
            
            te->hash = sub_tree_id;
            i = j; // Skip past all the files we just processed in the subdirectory
        }
    }

    // Serialize this level into binary format
    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;

    // Write it to the object store
    int res = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return res;
}

// Build a tree hierarchy from the current index
// Build a tree hierarchy from the current index
int tree_from_index(ObjectID *id_out) {
    // Allocate on the heap to prevent 5.6MB stack overflow
    Index *idx = malloc(sizeof(Index));
    if (!idx) return -1;
    
    // Load the current staging area
    if (index_load(idx) != 0) {
        free(idx);
        return -1;
    }

    // Handle the edgecase of an empty commit
    if (idx->count == 0) {
        Tree empty_tree;
        empty_tree.count = 0;
        void *data; 
        size_t len;
        if (tree_serialize(&empty_tree, &data, &len) != 0) {
            free(idx);
            return -1;
        }
        int res = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        free(idx);
        return res;
    }

    // Start the recursive build at the root (depth offset 0)
    int result = write_tree_level(idx, 0, idx->count, 0, id_out);
    free(idx);
    return result;
}
