/* Unit tests of the file helpers and of SHA-256. */
#ifndef _WIN32
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#endif

#include "internal.h"
#include "test.h"
#include "test_files.h"

static char *tmp;

static void test_sha256(void) {
    struct {
        const char *input, *digest;
    } cases[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    char hex[65];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rag_sha256_hex(cases[i].input, strlen(cases[i].input), hex);
        CHECK_STR(hex, cases[i].digest);
    }

    /* a million "a", fed in uneven pieces */
    static char a[1000];
    memset(a, 'a', sizeof(a));
    sha256_t s;
    unsigned char digest[32];
    rag_sha256_init(&s);
    for (size_t done = 0, step = 1; done < 1000000; step = step % 997 + 1) {
        size_t n = 1000000 - done < step ? 1000000 - done : step;
        rag_sha256_update(&s, a, n);
        done += n;
    }
    rag_sha256_final(&s, digest);
    static const char *digits = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[64] = '\0';
    CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_text_extensions(void) {
    CHECK(rag_fs_is_text_file("a.md"));
    CHECK(rag_fs_is_text_file("NOTES.MD"));
    CHECK(rag_fs_is_text_file("x.txt"));
    CHECK(rag_fs_is_text_file("readme.markdown"));
    CHECK(!rag_fs_is_text_file(".md"));
    CHECK(!rag_fs_is_text_file("a.mdx"));
    CHECK(!rag_fs_is_text_file("a.c"));
    CHECK(!rag_fs_is_text_file("txt"));
}

static void check_canonical(const char *input, const char *expected) {
    err_t e;
    rag_err_init(&e);
    char *got = rag_fs_canonical(input, &e);
    rag_err_free(&e);
    CHECK_STR(got, expected);
    free(got);
}

static void test_canonical(void) {
    write_file(path_of(tmp, "a.md"), "a");
    write_file(path_of(tmp, "sub/b.md"), "b");
    write_file(path_of(tmp, "with space/città è.md"), "c");
    bool links = make_link("a.md", path_of(tmp, "link.md"), false) && make_link("sub", path_of(tmp, "dirlink"), true);
    if (!links) printf("  (links cannot be made here: not tested)\n");

    char *cwd = current_dir();
    CHECK(change_dir(tmp));
    check_canonical("a.md", path_of(tmp, "a.md"));
    check_canonical("./sub/../a.md", path_of(tmp, "a.md"));
    check_canonical("sub/", path_of(tmp, "sub"));
    check_canonical(".", tmp);
    check_canonical("sub/..", tmp);
    /* spaces and characters that are not ASCII */
    check_canonical("with space/città è.md", path_of(tmp, "with space/città è.md"));
    check_canonical("with space", path_of(tmp, "with space"));
    /* files that do not exist */
    check_canonical("missing.md", path_of(tmp, "missing.md"));
    check_canonical("missing/dir/x.md", path_of(tmp, "missing/dir/x.md"));
    check_canonical("missing/../x.md", path_of(tmp, "x.md"));
    check_canonical("gone/", path_of(tmp, "gone"));
    if (links) {
        /* links to files are kept, links to directories are resolved */
        check_canonical("link.md", path_of(tmp, "link.md"));
        check_canonical("dirlink", path_of(tmp, "sub"));
        check_canonical("dirlink/b.md", path_of(tmp, "sub/b.md"));
        check_canonical("dirlink/new.md", path_of(tmp, "sub/new.md"));
    }
    /* names spelled as on the disk, where they do not depend on case (Windows, and macOS usually) */
    if (file_exists(path_of(tmp, "A.MD"))) {
        check_canonical("SUB/B.MD", path_of(tmp, "sub/b.md"));
        check_canonical("Sub", path_of(tmp, "sub"));
        check_canonical("WITH SPACE/CITTÀ È.MD", path_of(tmp, "with space/città è.md"));
        check_canonical("SUB/New.md", path_of(tmp, "sub/New.md")); /* a file that does not exist keeps its name */
    } else {
        printf("  (names depend on case here: their spelling is not tested)\n");
    }
#ifdef _WIN32
    /* both separators */
    check_canonical("sub\\b.md", path_of(tmp, "sub/b.md"));
    check_canonical(".\\sub/..\\a.md", path_of(tmp, "a.md"));
    check_canonical("WITH SPACE\\CITTÀ È.MD", path_of(tmp, "with space/città è.md"));
#endif
    CHECK(change_dir(cwd));
    free(cwd);

    check_canonical(path_of(tmp, "a.md"), path_of(tmp, "a.md"));
    err_t e;
    rag_err_init(&e);
#ifdef _WIN32
    /* the root of a drive, in any spelling */
    char drive[4] = {tmp[0], ':', '\\', '\0'};
    char lower_drive[4] = {(char)(tmp[0] - 'A' + 'a'), ':', '/', '\0'};
    check_canonical(drive, drive);
    check_canonical(lower_drive, drive);
    char *forward = dup_string(path_of(tmp, "sub/b.md"));
    for (char *p = forward; *p; p++)
        if (*p == '\\') *p = '/';
    check_canonical(forward, path_of(tmp, "sub/b.md"));
    free(forward);
#else
    check_canonical("/", "/");
    check_canonical("//", "/");

    /* the current directory does not exist anymore (Windows cannot remove it) */
    char *here = current_dir();
    const char *gone = path_of(tmp, "gone-cwd");
    CHECK(make_dir(gone));
    CHECK(change_dir(gone));
    CHECK(remove_dir(gone));
    char *probe = rag_os_getcwd(NULL); /* some systems still know the path */
    if (!probe) {
        CHECK(rag_fs_canonical("a.md", &e) == NULL);
        CHECK(e.pub.code == RAG_ERROR_IO);
        CHECK(strstr(e.pub.message, "current directory") != NULL);
    }
    free(probe);
    CHECK(change_dir(here));
    free(here);
#endif
    CHECK(rag_fs_canonical("", &e) == NULL);
    CHECK(e.pub.code == RAG_ERROR_INVALID);
    rag_err_free(&e);
}

/* Paths longer than the 260 characters of MAX_PATH work on Windows too. */
static void test_long_paths(void) {
    string_t path = {0};
    string_append(&path, tmp);
    string_append(&path, "/long");
    for (int i = 0; i < 12; i++) string_append(&path, "/directory-with-a-long-name");
    CHECK(path.len > 300);
    char *dir = dup_string(path_of(path.data, "x"));
    dir[strlen(dir) - 2] = '\0'; /* the same path, with the separators of the system */
    char *file = dup_string(path_of(dir, "note.md"));
    write_file(file, "a long way down");

    err_t e;
    rag_err_init(&e);
    fs_list_t list = {0};
    char *top = dup_string(path_of(tmp, "long"));
    CHECK(rag_fs_list_dir(top, &list, &e) == RAG_OK);
    CHECK_SIZE(list.count, 1);
    if (list.count == 1) CHECK_STR(list.items[0].path, file);
    rag_fs_list_free(&list);
    buf_t b = {0};
    CHECK(rag_fs_read_file(file, &b, &e) == RAG_OK);
    CHECK_STR(rag_buf_cstr(&b), "a long way down");
    rag_buf_free(&b);
    char *canonical = rag_fs_canonical(file, &e);
    CHECK_STR(canonical, file);
    free(canonical);
    rag_err_free(&e);
    free(top);
    free(file);
    free(dir);
    free(path.data);
}

static void test_list_dir(void) {
    char *root = rag_strdup(path_of(tmp, "notes"));
    const char *files[] = {
        "a.md", "b.txt", "c.c", ".hidden.md", ".git/x.md", "sub/d.markdown", "sub/deeper/e.MD", "with space/è ü.md"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) write_file(path_of(root, files[i]), "text");
    bool links = make_link("sub", path_of(root, "link_to_sub"), true) &&
                 make_link("a.md", path_of(root, "filelink.md"), false) &&
                 make_link("nowhere.md", path_of(root, "broken.md"), false);
#ifdef _WIN32
    /* hidden is an attribute on Windows */
    write_file(path_of(root, "attribute.md"), "text");
    wchar_t *hidden = test_wide(path_of(root, "attribute.md"));
    CHECK(SetFileAttributesW(hidden, FILE_ATTRIBUTE_HIDDEN));
    free(hidden);
#endif

    fs_list_t list = {0};
    err_t e;
    rag_err_init(&e);
    CHECK(rag_fs_list_dir(root, &list, &e) == RAG_OK);
    const char *expected[6];
    size_t count = 0;
    expected[count++] = "a.md";
    expected[count++] = "b.txt";
    if (links) expected[count++] = "filelink.md";
    expected[count++] = "sub/d.markdown";
    expected[count++] = "sub/deeper/e.MD";
    expected[count++] = "with space/è ü.md";
    CHECK_SIZE(list.count, count);
    for (size_t i = 0; i < list.count && i < count; i++) {
        CHECK_STR(list.items[i].path, path_of(root, expected[i]));
        CHECK(list.items[i].error == NULL);
    }
    rag_fs_list_free(&list);

#ifndef _WIN32
    /* unreadable directories are reported (root can read everything) */
    if (geteuid() != 0) {
        char *locked = rag_strdup(path_of(root, "locked"));
        write_file(path_of(locked, "f.md"), "x");
        chmod(locked, 0);
        CHECK(rag_fs_list_dir(root, &list, &e) == RAG_OK);
        size_t errors = 0;
        for (size_t i = 0; i < list.count; i++) {
            if (list.items[i].error) {
                errors++;
                CHECK_STR(list.items[i].path, locked);
            }
        }
        CHECK_SIZE(errors, 1);
        rag_fs_list_free(&list);
        chmod(locked, 0755);
        free(locked);
    }
#endif

    /* an empty directory */
    const char *empty = path_of(tmp, "empty");
    make_dir(empty);
    CHECK(rag_fs_list_dir(empty, &list, &e) == RAG_OK);
    CHECK_SIZE(list.count, 0);
    rag_fs_list_free(&list);

    CHECK(rag_fs_list_dir(path_of(root, "a.md"), &list, &e) == RAG_ERROR_INVALID);
    CHECK(rag_fs_list_dir(path_of(root, "missing"), &list, &e) == RAG_ERROR_NOT_FOUND);
    CHECK(strstr(e.pub.message, "missing") != NULL);
    CHECK_SIZE(list.count, 0);
    rag_err_free(&e);
    free(root);
}

static void test_read_file(void) {
    const char *path = path_of(tmp, "content.txt");
    write_file(path, "hello\nworld\n");
    buf_t b = {0};
    err_t e;
    rag_err_init(&e);
    CHECK(rag_fs_read_file(path, &b, &e) == RAG_OK);
    CHECK_STR(rag_buf_cstr(&b), "hello\nworld\n");
    rag_buf_free(&b);

    fs_info_t info;
    CHECK(rag_fs_stat(path, &info, &e) == RAG_OK);
    CHECK(info.is_file && !info.is_dir);
    CHECK(info.size == 12);
    CHECK(rag_fs_stat(tmp, &info, &e) == RAG_OK);
    CHECK(info.is_dir && !info.is_file);
    CHECK(rag_fs_stat(path_of(tmp, "nothing"), &info, &e) == RAG_ERROR_NOT_FOUND);

    CHECK(rag_fs_read_file(path_of(tmp, "nothing"), &b, &e) == RAG_ERROR_IO);
    CHECK(strstr(e.pub.message, "nothing") != NULL);
    rag_buf_free(&b);
    rag_err_free(&e);
}

int main(void) {
    tmp = make_temp_dir();
    RUN(test_sha256);
    RUN(test_text_extensions);
    RUN(test_canonical);
    RUN(test_long_paths);
    RUN(test_list_dir);
    RUN(test_read_file);
    remove_tree(tmp);
    free(tmp);
    return test_report();
}
