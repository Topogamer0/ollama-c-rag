/* Unit tests of the file helpers and of SHA-256. */
#define _XOPEN_SOURCE 700

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
        sha256_hex(cases[i].input, strlen(cases[i].input), hex);
        CHECK_STR(hex, cases[i].digest);
    }

    /* a million "a", fed in uneven pieces */
    static char a[1000];
    memset(a, 'a', sizeof(a));
    sha256_t s;
    unsigned char digest[32];
    sha256_init(&s);
    for (size_t done = 0, step = 1; done < 1000000; step = step % 997 + 1) {
        size_t n = 1000000 - done < step ? 1000000 - done : step;
        sha256_update(&s, a, n);
        done += n;
    }
    sha256_final(&s, digest);
    static const char *digits = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[64] = '\0';
    CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_text_extensions(void) {
    CHECK(fs_is_text_file("a.md"));
    CHECK(fs_is_text_file("NOTES.MD"));
    CHECK(fs_is_text_file("x.txt"));
    CHECK(fs_is_text_file("readme.markdown"));
    CHECK(!fs_is_text_file(".md"));
    CHECK(!fs_is_text_file("a.mdx"));
    CHECK(!fs_is_text_file("a.c"));
    CHECK(!fs_is_text_file("txt"));
}

static void check_canonical(const char *input, const char *expected) {
    char *got = fs_canonical(input);
    CHECK_STR(got, expected);
    free(got);
}

static void test_canonical(void) {
    write_file(path_of(tmp, "a.md"), "a");
    write_file(path_of(tmp, "sub/b.md"), "b");
    CHECK(symlink("a.md", path_of(tmp, "link.md")) == 0);
    CHECK(symlink("sub", path_of(tmp, "dirlink")) == 0);

    char *cwd = getcwd(NULL, 0);
    CHECK(chdir(tmp) == 0);
    check_canonical("a.md", path_of(tmp, "a.md"));
    check_canonical("./sub/../a.md", path_of(tmp, "a.md"));
    check_canonical("sub/", path_of(tmp, "sub"));
    check_canonical(".", tmp);
    check_canonical("sub/..", tmp);
    /* files that do not exist */
    check_canonical("missing.md", path_of(tmp, "missing.md"));
    check_canonical("missing/dir/x.md", path_of(tmp, "missing/dir/x.md"));
    check_canonical("missing/../x.md", path_of(tmp, "x.md"));
    check_canonical("gone/", path_of(tmp, "gone"));
    /* links to files are kept, links to directories are resolved */
    check_canonical("link.md", path_of(tmp, "link.md"));
    check_canonical("dirlink", path_of(tmp, "sub"));
    check_canonical("dirlink/b.md", path_of(tmp, "sub/b.md"));
    check_canonical("dirlink/new.md", path_of(tmp, "sub/new.md"));
    CHECK(chdir(cwd) == 0);
    free(cwd);

    check_canonical(path_of(tmp, "a.md"), path_of(tmp, "a.md"));
    check_canonical("/", "/");
    check_canonical("//", "/");
    CHECK(fs_canonical("") == NULL);
}

static void test_list_dir(void) {
    char *root = rag_strdup(path_of(tmp, "notes"));
    const char *files[] = {"a.md", "b.txt", "c.c", ".hidden.md", ".git/x.md", "sub/d.markdown", "sub/deeper/e.MD"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) write_file(path_of(root, files[i]), "text");
    CHECK(symlink("sub", path_of(root, "link_to_sub")) == 0);
    CHECK(symlink("a.md", path_of(root, "filelink.md")) == 0);
    CHECK(symlink("nowhere.md", path_of(root, "broken.md")) == 0);

    fs_list_t list = {0};
    err_t e;
    err_init(&e);
    CHECK(fs_list_dir(root, &list, &e) == RAG_OK);
    const char *expected[] = {"a.md", "b.txt", "filelink.md", "sub/d.markdown", "sub/deeper/e.MD"};
    CHECK_SIZE(list.count, 5);
    for (size_t i = 0; i < list.count && i < 5; i++) {
        CHECK_STR(list.items[i].path, path_of(root, expected[i]));
        CHECK(list.items[i].error == NULL);
    }
    fs_list_free(&list);

    /* unreadable directories are reported (root can read everything) */
    if (geteuid() != 0) {
        char *locked = rag_strdup(path_of(root, "locked"));
        write_file(path_of(locked, "f.md"), "x");
        chmod(locked, 0);
        CHECK(fs_list_dir(root, &list, &e) == RAG_OK);
        size_t errors = 0;
        for (size_t i = 0; i < list.count; i++) {
            if (list.items[i].error) {
                errors++;
                CHECK_STR(list.items[i].path, locked);
            }
        }
        CHECK_SIZE(errors, 1);
        fs_list_free(&list);
        chmod(locked, 0755);
        free(locked);
    }

    CHECK(fs_list_dir(path_of(root, "a.md"), &list, &e) == RAG_ERROR_INVALID);
    CHECK(fs_list_dir(path_of(root, "missing"), &list, &e) == RAG_ERROR_NOT_FOUND);
    CHECK(strstr(e.pub.message, "missing") != NULL);
    CHECK_SIZE(list.count, 0);
    err_free(&e);
    free(root);
}

static void test_read_file(void) {
    const char *path = path_of(tmp, "content.txt");
    write_file(path, "hello\nworld\n");
    buf_t b = {0};
    err_t e;
    err_init(&e);
    CHECK(fs_read_file(path, &b, &e) == RAG_OK);
    CHECK_STR(buf_cstr(&b), "hello\nworld\n");
    buf_free(&b);

    fs_info_t info;
    CHECK(fs_stat(path, &info, &e) == RAG_OK);
    CHECK(info.is_file && !info.is_dir);
    CHECK(info.size == 12);
    CHECK(fs_stat(tmp, &info, &e) == RAG_OK);
    CHECK(info.is_dir && !info.is_file);
    CHECK(fs_stat(path_of(tmp, "nothing"), &info, &e) == RAG_ERROR_NOT_FOUND);

    CHECK(fs_read_file(path_of(tmp, "nothing"), &b, &e) == RAG_ERROR_IO);
    CHECK(strstr(e.pub.message, "nothing") != NULL);
    buf_free(&b);
    err_free(&e);
}

int main(void) {
    tmp = make_temp_dir();
    RUN(test_sha256);
    RUN(test_text_extensions);
    RUN(test_canonical);
    RUN(test_list_dir);
    RUN(test_read_file);
    remove_tree(tmp);
    free(tmp);
    return test_report();
}
