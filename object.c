// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str;
    
    if (type == OBJ_BLOB) type_str = "blob";
    else if (type == OBJ_TREE) type_str = "tree";
    else if (type == OBJ_COMMIT) type_str = "commit";
    else return -1; 

    // 1. Build the full object: header + data
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    size_t total_len = header_len + 1 + len; // +1 for the '\0' byte

    char *full_obj = malloc(total_len);
    if (!full_obj) return -1;

    memcpy(full_obj, header, header_len);
    full_obj[header_len] = '\0';
    memcpy(full_obj + header_len + 1, data, len);

    // 2. Compute SHA-256 hash of the FULL object
    compute_hash(full_obj, total_len, id_out);

    // 3. Deduplication: Check if object already exists
    if (object_exists(id_out)) {
        free(full_obj);
        return 0; // Already saved!
    }

    // 4. Create shard directory (.pes/objects/XX/)
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    
    char shard_dir[256];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755); // We ignore the return value; it fails harmlessly if it already exists

    // 5. Write to a temporary file
    char temp_path[512]; // FIXED: Buffer increased to prevent compilation warnings
    snprintf(temp_path, sizeof(temp_path), "%s/temp_XXXXXX", shard_dir);
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }

    if (write(fd, full_obj, total_len) != (ssize_t)total_len) {
        close(fd);
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    // 6. fsync() the temporary file to ensure data reaches disk
    fsync(fd);
    close(fd);

    // 7. rename() the temp file to the final path (atomic operation)
    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));
    
    if (rename(temp_path, final_path) != 0) {
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    // 8. Open and fsync() the shard directory to persist the rename
    int dir_fd = open(shard_dir, O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    free(full_obj);
    return 0;
}

// Read an object from the store.
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // 1. Build the file path
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return -1;
    }

    char *buffer = malloc(file_size);
    if (!buffer) {
        fclose(f);
        return -1;
    }

    if (fread(buffer, 1, file_size, f) != (size_t)file_size) {
        free(buffer);
        fclose(f);
        return -1;
    }
    fclose(f);

    // 4. Verify integrity: recompute hash and compare
    ObjectID computed_hash;
    compute_hash(buffer, file_size, &computed_hash);
    if (memcmp(computed_hash.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1; // Data corruption detected
    }

    // 3. Parse the header to extract the type string and size
    char *null_byte = memchr(buffer, '\0', file_size);
    if (!null_byte) {
        free(buffer);
        return -1; // Corrupted format, missing null terminator
    }

    *len_out = file_size - (null_byte - buffer) - 1;
    
    char type_str[16];
    size_t parsed_len;
    if (sscanf(buffer, "%15s %zu", type_str, &parsed_len) != 2) {
        free(buffer);
        return -1;
    }

    // 5. Set *type_out
    if (strcmp(type_str, "blob") == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree") == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else {
        free(buffer);
        return -1; // Unknown object type
    }

    // 6. Allocate buffer, copy the data portion, and set *data_out
    *data_out = malloc(*len_out);
    if (!*data_out) {
        free(buffer);
        return -1;
    }
    memcpy(*data_out, null_byte + 1, *len_out);

    free(buffer);
    return 0;
}
