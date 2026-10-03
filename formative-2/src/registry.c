#include "registry.h"

#include <stdio.h>
#include <string.h>

#define LINE_BUF_LEN 256

static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

/* Splits `line` in place on ',' into at most 3 fields. Returns the number of
 * fields actually found. A line with more than 2 commas keeps everything
 * after the 2nd comma in the 3rd field (author/full_name are free text and
 * must not themselves contain commas per the registry format). */
static int split_fields(char *line, char *fields[3]) {
    int count = 0;
    char *p = line;
    fields[count++] = p;
    while (count < 3) {
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
        fields[count++] = p;
    }
    return count;
}

static FILE *open_registry_file(const char *path, const char *label) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ERROR: %s file '%s' not found or could not be opened.\n", label, path);
    }
    return f;
}

int registry_load(Registry *reg, const char *books_path, const char *members_path) {
    memset(reg, 0, sizeof(*reg));
    char line[LINE_BUF_LEN];

    FILE *bf = open_registry_file(books_path, "Book registry");
    if (!bf) return 0;

    while (fgets(line, sizeof(line), bf)) {
        trim_newline(line);
        if (line[0] == '\0') continue;

        if (reg->book_count >= MAX_BOOKS) {
            fprintf(stderr, "WARNING: %s has more than %d entries; extra entries ignored.\n",
                    books_path, MAX_BOOKS);
            break;
        }

        char *fields[3];
        int n = split_fields(line, fields);
        if (n != 3) {
            fprintf(stderr, "WARNING: malformed line in %s (expected 3 comma-separated fields, got %d): %s\n",
                    books_path, n, line);
            continue;
        }
        if (strlen(fields[0]) >= BOOK_ID_LEN ||
            strlen(fields[1]) >= BOOK_TITLE_LEN ||
            strlen(fields[2]) >= BOOK_AUTHOR_LEN) {
            fprintf(stderr, "WARNING: field too long in %s, line skipped: %s\n", books_path, line);
            continue;
        }

        Book *b = &reg->books[reg->book_count];
        snprintf(b->book_id, BOOK_ID_LEN, "%s", fields[0]);
        snprintf(b->title, BOOK_TITLE_LEN, "%s", fields[1]);
        snprintf(b->author, BOOK_AUTHOR_LEN, "%s", fields[2]);
        reg->book_count++;
    }
    fclose(bf);

    if (reg->book_count == 0) {
        fprintf(stderr, "ERROR: Book registry '%s' is empty or contains no valid records.\n", books_path);
        return 0;
    }

    FILE *mf = open_registry_file(members_path, "Member registry");
    if (!mf) return 0;

    while (fgets(line, sizeof(line), mf)) {
        trim_newline(line);
        if (line[0] == '\0') continue;

        if (reg->member_count >= MAX_MEMBERS) {
            fprintf(stderr, "WARNING: %s has more than %d entries; extra entries ignored.\n",
                    members_path, MAX_MEMBERS);
            break;
        }

        char *fields[3];
        int n = split_fields(line, fields);
        if (n != 3) {
            fprintf(stderr, "WARNING: malformed line in %s (expected 3 comma-separated fields, got %d): %s\n",
                    members_path, n, line);
            continue;
        }
        if (strlen(fields[0]) >= MEMBER_ID_LEN ||
            strlen(fields[1]) >= MEMBER_NAME_LEN ||
            strlen(fields[2]) >= MEMBER_COURSE_LEN) {
            fprintf(stderr, "WARNING: field too long in %s, line skipped: %s\n", members_path, line);
            continue;
        }

        Member *m = &reg->members[reg->member_count];
        snprintf(m->member_id, MEMBER_ID_LEN, "%s", fields[0]);
        snprintf(m->full_name, MEMBER_NAME_LEN, "%s", fields[1]);
        snprintf(m->course_code, MEMBER_COURSE_LEN, "%s", fields[2]);
        reg->member_count++;
    }
    fclose(mf);

    if (reg->member_count == 0) {
        fprintf(stderr, "ERROR: Member registry '%s' is empty or contains no valid records.\n", members_path);
        return 0;
    }

    return 1;
}

const Book *registry_find_book(const Registry *reg, const char *book_id) {
    for (size_t i = 0; i < reg->book_count; i++) {
        if (strcmp(reg->books[i].book_id, book_id) == 0) return &reg->books[i];
    }
    return NULL;
}

const Member *registry_find_member(const Registry *reg, const char *member_id) {
    for (size_t i = 0; i < reg->member_count; i++) {
        if (strcmp(reg->members[i].member_id, member_id) == 0) return &reg->members[i];
    }
    return NULL;
}
