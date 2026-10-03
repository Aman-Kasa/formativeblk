#ifndef REGISTRY_H
#define REGISTRY_H

#include <stddef.h>

#define BOOK_ID_LEN 20
#define BOOK_TITLE_LEN 80
#define BOOK_AUTHOR_LEN 50
#define MEMBER_ID_LEN 20
#define MEMBER_NAME_LEN 50
#define MEMBER_COURSE_LEN 10

/* Registries are loaded once at startup from a bounded lab dataset (assignment
 * examples: 3 books, 3 members). Fixed-size arrays avoid realloc bookkeeping
 * for data that is read-only for the lifetime of the program. */
#define MAX_BOOKS 256
#define MAX_MEMBERS 256

typedef struct {
    char book_id[BOOK_ID_LEN];
    char title[BOOK_TITLE_LEN];
    char author[BOOK_AUTHOR_LEN];
} Book;

typedef struct {
    char member_id[MEMBER_ID_LEN];
    char full_name[MEMBER_NAME_LEN];
    char course_code[MEMBER_COURSE_LEN];
} Member;

typedef struct {
    Book books[MAX_BOOKS];
    size_t book_count;
    Member members[MAX_MEMBERS];
    size_t member_count;
} Registry;

/* Loads books_path and members_path into `reg`. Returns 1 on success, 0 on
 * failure. On failure a specific reason (missing file / empty file / no
 * valid records) is printed to stderr. Malformed individual lines are
 * skipped with a warning rather than aborting the whole load. */
int registry_load(Registry *reg, const char *books_path, const char *members_path);

const Book *registry_find_book(const Registry *reg, const char *book_id);
const Member *registry_find_member(const Registry *reg, const char *member_id);

#endif
