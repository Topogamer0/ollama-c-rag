#!/usr/bin/env python3
"""
Tests of the ragc command line tool, run by mock_server.py:

    mock_server.py python3 test_cli.py <path to ragc>

Set RAGC_TEST_WINDOWS=1 when <path to ragc> runs a Windows ragc.exe from
another system (a script that runs it with Wine): it shows paths with "\\".
"""

import json
import os
import subprocess
import sys
import tempfile
import urllib.request

RAGC = os.path.abspath(sys.argv[1])
HOST = os.environ['OLLAMA_HOST']
WINDOWS = os.name == 'nt' or os.environ.get('RAGC_TEST_WINDOWS') == '1'
failures = 0
checks = 0


def p(path):
    """A relative path as ragc shows it: with the separators of its system."""
    return path.replace('/', '\\') if WINDOWS else path


def check(condition, message, result=None):
    global failures, checks
    checks += 1
    if not condition:
        failures += 1
        print(f'check failed: {message}', file=sys.stderr)
        if result is not None:
            print(f'  exit code: {result.returncode}\n  stdout: {result.stdout!r}\n  stderr: {result.stderr!r}',
                  file=sys.stderr)


def ragc(*args, cwd, env=None):
    clean = {k: v for k, v in os.environ.items() if not k.startswith('RAGC_')}
    clean.update(env or {})
    # ragc writes UTF-8, whatever the code page of the system
    return subprocess.run([RAGC, *args], cwd=cwd, env=clean, capture_output=True, encoding='utf-8', errors='replace')


def mock(path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    with urllib.request.urlopen(f'http://{HOST}{path}', data) as response:
        return json.loads(response.read())


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:  # the same bytes on every system
        f.write(text)


def main():
    # on Windows, an antivirus may still hold a file that was just closed
    cleanup = {'ignore_cleanup_errors': True} if sys.version_info >= (3, 10) else {}
    with tempfile.TemporaryDirectory(**cleanup) as tmp:
        tmp = os.path.realpath(tmp)
        notes = os.path.join(tmp, 'notes')
        write(os.path.join(notes, 'fruit.md'),
              '# Fruit\n\nBananas are yellow fruits rich in potassium. Monkeys love bananas.\n\n'
              'Apples can be red or green. An apple a day keeps the doctor away.\n')
        write(os.path.join(notes, 'space.md'),
              '# Space\n\nThe Moon orbits the Earth every 27 days.\n\nMars is the red planet, with two small moons.\n')
        write(os.path.join(notes, 'sub', 'cooking.txt'), 'Pasta needs salted boiling water.\n')
        with open(os.path.join(notes, 'image.txt'), 'wb') as f:
            f.write(b'\x89PNG\x00\x00')
        os.makedirs(os.path.join(tmp, 'nothing'))

        # usage
        r = ragc('--help', cwd=tmp)
        check(r.returncode == 0 and 'usage: ragc' in r.stdout, '--help', r)
        r = ragc('--version', cwd=tmp)
        check(r.returncode == 0 and r.stdout.startswith('ragc 0.'), '--version', r)
        for args, error in [((), 'usage'), (('frobnicate',), 'unknown command'), (('--nope', 'stats'), 'unknown option'),
                            (('-k', 'x', 'search', 'a'), 'invalid number'), (('-k',), 'needs a value'),
                            (('-s', '2', 'search', 'a'), 'between -1 and 1'), (('search',), 'what are you'),
                            (('--quiet=yes', 'stats'), 'unknown option')]:
            r = ragc(*args, cwd=tmp)
            check(r.returncode == 2 and error in r.stderr, f'usage error {args}', r)

        # the database must exist to search
        r = ragc('stats', cwd=tmp)
        check(r.returncode == 1 and 'does not exist' in r.stderr and 'ragc index' in r.stderr, 'missing database', r)
        check(not os.path.exists(os.path.join(tmp, 'ragc.db')), 'the database is not created by stats')

        # index
        r = ragc('-e', 'mock-embed', '--chunk-size', '80', '--chunk-overlap', '10', 'index', 'notes', cwd=tmp)
        check(r.returncode == 0, 'index', r)
        check(f'indexed  {p("notes/fruit.md")} (2 chunks)' in r.stderr, 'index progress', r)
        check(f'skipped  {p("notes/image.txt")}: ' in r.stderr and 'binary' in r.stderr, 'binary files are skipped', r)
        check(tmp not in r.stderr and "'/" not in r.stderr, 'paths are relative', r)
        check('3 indexed, 0 unchanged, 0 removed, 1 skipped' in r.stdout, 'index summary', r)

        r = ragc('-q', 'index', 'notes', cwd=tmp)
        check(r.returncode == 0 and r.stderr == '' and '0 indexed, 3 unchanged' in r.stdout, 'quiet index', r)

        # stats
        r = ragc('stats', cwd=tmp)
        check(r.returncode == 0, 'stats', r)
        check('mock-embed (256 dimensions)' in r.stdout, 'stats model', r)
        check('files            3' in r.stdout, 'stats files', r)
        check('80 characters, 10 shared' in r.stdout, 'stats chunking', r)

        # search, with or without quotes
        r = ragc('search', 'yellow', 'bananas', 'potassium', cwd=tmp)
        check(r.returncode == 0 and r.stdout.startswith(f'1. {p("notes/fruit.md")}:1-3  (score '), 'search', r)
        check('   Bananas are yellow' in r.stdout and '\n2. ' in r.stdout, 'search results', r)
        r = ragc('-k', '1', 'search', 'Mars red planet', cwd=tmp)
        check(r.returncode == 0 and p('notes/space.md') in r.stdout and '\n2. ' not in r.stdout, 'top-k', r)
        r = ragc('--min-score=0.999', 'search', 'bananas', cwd=tmp)
        check(r.returncode == 0 and r.stdout == '' and 'nothing found' in r.stderr, 'min-score', r)
        r = ragc('search', 'bananas', cwd=notes)
        check(r.returncode == 1 and 'does not exist' in r.stderr, 'the database is in the current directory', r)
        r = ragc('--db', '../ragc.db', 'search', 'bananas', cwd=notes)
        check(r.returncode == 0 and r.stdout.startswith('1. fruit.md:'), 'paths relative to the current directory', r)

        # ask
        mock('/_reset', {})
        r = ragc('-m', 'mock-chat', 'ask', 'What', 'color', 'are', 'bananas?', cwd=tmp)
        check(r.returncode == 0, 'ask', r)
        check(r.stdout.startswith('Found 5 sources [1]. You asked: What color are bananas?\n'), 'answer', r)
        check(f'\nSources:\n  [1] {p("notes/fruit.md")}:1-3 (score ' in r.stdout, 'sources', r)
        last = mock('/_last')
        body = json.loads(last['body'])
        check(last['path'] == '/api/chat' and body['model'] == 'mock-chat', 'chat request')
        check(body['messages'][0]['role'] == 'system' and 'cite' in body['messages'][0]['content'], 'system prompt')
        prompt = body['messages'][1]['content']
        check(prompt.startswith('Passages from the documents:\n\n[1] fruit.md, lines 1-3\n# Fruit'), 'prompt')
        check(prompt.endswith('\n\nQuestion: What color are bananas?'), 'question in the prompt')
        check(mock('/_stats')['chat_requests'] == 1, 'one chat request')

        r = ragc('-q', '-k', '2', '--system', 'Be brief.', 'ask', 'bananas', cwd=tmp)
        check(r.returncode == 0 and 'Found 2 sources' in r.stdout and 'Sources:' not in r.stdout, 'quiet ask', r)
        body = json.loads(mock('/_last')['body'])
        check(body['messages'][0]['content'] == 'Be brief.' and body['model'] == 'gemma3', 'system option', r)
        r = ragc('--system', '', 'ask', 'bananas', cwd=tmp, env={'RAGC_CHAT_MODEL': 'env-chat'})
        body = json.loads(mock('/_last')['body'])
        check(len(body['messages']) == 1 and body['model'] == 'env-chat', 'no system prompt, model from env', r)

        r = ragc('-m', 'missing', 'ask', 'bananas', cwd=tmp)
        check(r.returncode == 1 and 'ollama pull missing' in r.stderr, 'missing chat model', r)
        r = ragc('-m', 'midstream-error', 'ask', 'bananas', cwd=tmp)
        check(r.returncode == 1 and 'something went wrong' in r.stderr, 'error while streaming', r)
        r = ragc('-H', 'http://127.0.0.1:1', 'ask', 'bananas', cwd=tmp)
        check(r.returncode == 1 and 'Failed to connect' in r.stderr, 'Ollama not running', r)

        # the question is the rest of the line, words starting with "-" included
        r = ragc('search', 'bananas', '-40', 'degrees', cwd=tmp)
        check(r.returncode == 0 and r.stdout.startswith('1. '), 'search with "-40" in the question', r)
        r = ragc('-m', 'mock-chat', 'ask', 'what', 'does', '-h', 'do', cwd=tmp)
        check(r.returncode == 0 and 'You asked: what does -h do' in r.stdout, 'ask with "-h" in the question', r)
        r = ragc('-m', 'mock-chat', 'ask', '--', '-q', 'is', 'a', 'word', cwd=tmp)
        check(r.returncode == 0 and 'You asked: -q is a word' in r.stdout, '"--" after the command', r)
        r = ragc('search', 'bananas', cwd=tmp)
        check('\n   \n' not in r.stdout, 'no spaces on the empty lines of the results', r)
        with open(os.path.join(tmp, 'crlf.txt'), 'wb') as f:
            f.write(b'Windows line ends.\r\n\r\nKiwis are green.\r\n')
        r = ragc('--db', 'crlf.db', '-e', 'mock-embed', 'index', 'crlf.txt', cwd=tmp)
        r = ragc('--db', 'crlf.db', 'search', 'kiwis', cwd=tmp)
        check(r.returncode == 0 and '   Windows line ends.\n\n   Kiwis are green.\n' in r.stdout and '\r' not in r.stdout,
              'results of a file with CRLF line ends', r)

        # a minimum score of 0 drops the negative scores, NaN is refused
        r = ragc('-k', '100', 'search', 'bananas', cwd=tmp)
        scores = [float(line.rsplit('score ', 1)[1].rstrip(')')) for line in r.stdout.splitlines() if '(score ' in line]
        r = ragc('-k', '100', '-s', '0', 'search', 'bananas', cwd=tmp)
        kept = [float(line.rsplit('score ', 1)[1].rstrip(')')) for line in r.stdout.splitlines() if '(score ' in line]
        check(kept == [x for x in scores if x >= 0] and all(x >= 0 for x in kept), '-s 0', r)
        r = ragc('-s', 'nan', 'search', 'bananas', cwd=tmp)
        check(r.returncode == 2 and 'between -1 and 1' in r.stderr, '-s nan', r)

        # empty arguments are invalid arguments
        for args in [('search', ''), ('ask', ''), ('--db', '', 'stats'), ('index', '')]:
            r = ragc(*args, cwd=tmp)
            check(r.returncode == 2 and 'ragc --help' in r.stderr, f'empty argument {args}', r)

        # values out of their range are invalid arguments too
        for args in [('--batch-size', '65537', 'index', 'notes'), ('--chunk-size', '100', '--chunk-overlap', '100', 'stats'),
                     ('index', '', 'notes'), ('remove', 'notes', ''), ('stats', '')]:
            r = ragc(*args, cwd=tmp)
            check(r.returncode == 2 and 'ragc --help' in r.stderr, f'invalid argument {args}', r)

        # a huge top-k returns everything
        r = ragc('-k', '1152921504606846977', 'search', 'bananas', cwd=tmp)
        check(r.returncode == 0 and '\n5. ' in r.stdout, 'huge top-k', r)
        r = ragc('--batch-size', '1152921504606846976', 'index', 'notes', cwd=tmp)
        check(r.returncode == 2 and '--batch-size' in r.stderr, 'huge batch size', r)

        # an output that cannot be written is an error
        if os.path.exists('/dev/full') and not WINDOWS:
            with open('/dev/full', 'w') as full:
                r = subprocess.run([RAGC, 'stats'], cwd=tmp, stdout=full, stderr=subprocess.PIPE, text=True)
            check(r.returncode == 1 and 'cannot write' in r.stderr, 'stdout full', r)

        # another embedding model
        r = ragc('-e', 'other-embed', 'search', 'bananas', cwd=tmp)
        check(r.returncode == 1 and "'mock-embed'" in r.stderr and "'other-embed'" in r.stderr and '--db' in r.stderr,
              'model mismatch', r)

        # changes
        write(os.path.join(notes, 'space.md'), '# Space\n\nJupiter is the largest planet.\n')
        os.remove(os.path.join(notes, 'sub', 'cooking.txt'))
        r = ragc('index', 'notes', cwd=tmp)
        check(r.returncode == 0 and '1 indexed, 1 unchanged, 1 removed' in r.stdout, 'incremental index', r)
        check(f'removed  {p("notes/sub/cooking.txt")}' in r.stderr, 'removed file', r)

        # a single file, any extension
        write(os.path.join(tmp, 'todo.org'), 'Buy tomatoes and basil.')
        r = ragc('index', 'todo.org', cwd=tmp)
        check(r.returncode == 0 and '1 indexed' in r.stdout and 'indexed  todo.org (1 chunk)' in r.stderr,
              'index a file', r)
        r = ragc('index', 'missing.md', cwd=tmp)
        check(r.returncode == 1 and 'missing.md' in r.stderr, 'index a missing file', r)

        # a file that cannot be indexed does not stop the others
        write(os.path.join(tmp, 'other.md'), 'Another note about carrots.')
        r = ragc('index', 'nowhere', 'other.md', cwd=tmp)
        check(r.returncode == 1 and 'nowhere' in r.stderr and '1 indexed' in r.stdout, 'index after a missing path', r)
        r = ragc('remove', 'other.md', cwd=tmp)
        r = ragc('index', 'notes/image.txt', 'other.md', cwd=tmp)
        check(r.returncode == 1 and 'binary' in r.stderr and '1 indexed' in r.stdout and '1 skipped' in r.stdout,
              'index several files, one of them binary', r)

        # paths are made relative only where they start (on Windows, ":" cannot be in a name)
        if not WINDOWS:
            deep = os.path.join(tmp, 'm' + tmp, 'w')
            os.makedirs(deep)
            os.makedirs(os.path.join(tmp, 'w'), exist_ok=True)
            with open(os.path.join(deep, 'bad.txt'), 'wb') as f:
                f.write(b'\x00')
            r = ragc('--db', '../ragc.db', 'index', os.path.join(deep, 'bad.txt'), cwd=os.path.join(tmp, 'w'))
            check(r.returncode == 1 and os.path.join(deep, 'bad.txt') in r.stderr, 'paths in messages', r)

        # remove
        r = ragc('remove', 'notes/fruit.md', 'todo.org', cwd=tmp)
        check(r.returncode == 0 and '2 files removed' in r.stdout and f'removed  {p("notes/fruit.md")}' in r.stderr,
              'remove', r)
        r = ragc('remove', 'notes/fruit.md', cwd=tmp)
        check(r.returncode == 1 and 'not in the index' in r.stderr, 'remove twice', r)
        r = ragc('remove', 'notes', cwd=tmp)
        check(r.returncode == 0 and '1 file removed' in r.stdout, 'remove a directory', r)

        # an empty index
        r = ragc('--db', 'empty.db', 'index', 'nothing', cwd=tmp)
        check(r.returncode == 0 and '0 indexed' in r.stdout, 'index an empty directory', r)
        r = ragc('--db', 'empty.db', 'ask', 'bananas', cwd=tmp)
        check(r.returncode == 1 and 'contains no documents' in r.stderr, 'ask an empty index', r)

        # the database from the environment
        r = ragc('index', 'notes', cwd=tmp, env={'RAGC_DB': 'env.db', 'RAGC_EMBED_MODEL': 'mock-embed'})
        check(r.returncode == 0 and os.path.exists(os.path.join(tmp, 'env.db')), 'RAGC_DB', r)
        r = ragc('stats', cwd=tmp, env={'RAGC_DB': 'env.db'})
        check(r.returncode == 0 and 'env.db' in r.stdout and 'mock-embed' in r.stdout, 'stats of RAGC_DB', r)

        # spaces and characters that are not ASCII, in paths, arguments and the environment
        write(os.path.join(tmp, 'note città', 'perché è.md'), '# Città\n\nCaffè e cornetti a colazione, perché no.\n')
        r = ragc('--db', 'unicode.db', '-e', 'mock-embed', 'index', 'note città', cwd=tmp)
        check(r.returncode == 0 and f'indexed  {p("note città/perché è.md")} (1 chunk)' in r.stderr, 'index città', r)
        r = ragc('--db', 'unicode.db', 'search', 'caffè', 'colazione', cwd=tmp)
        check(r.returncode == 0 and r.stdout.startswith(f'1. {p("note città/perché è.md")}:1-3') and
              'Caffè e cornetti' in r.stdout, 'search caffè', r)
        r = ragc('-m', 'mock-chat', 'ask', 'perché', 'il', 'caffè?', cwd=tmp, env={'RAGC_DB': 'unicode.db'})
        check(r.returncode == 0 and 'You asked: perché il caffè?' in r.stdout and p('note città/perché è.md') in r.stdout,
              'ask in Italian', r)
        r = ragc('stats', cwd=tmp, env={'RAGC_DB': 'banca dati è.db'})
        check(r.returncode == 1 and 'banca dati è.db' in r.stderr, 'RAGC_DB with characters that are not ASCII', r)
        r = ragc('--db', 'unicode.db', 'remove', 'note città/perché è.md', cwd=tmp)
        check(r.returncode == 0 and '1 file removed' in r.stdout, 'remove città', r)
        if WINDOWS:
            # both separators, any case
            r = ragc('--db', 'unicode.db', 'index', 'NOTE CITTÀ\\PERCHÉ È.MD', cwd=tmp)
            check(r.returncode == 0 and f'indexed  {p("note città/perché è.md")}' in r.stderr, 'index in capitals', r)
            r = ragc('--db', 'unicode.db', 'remove', 'note città\\perché è.md', cwd=tmp)
            check(r.returncode == 0 and '1 file removed' in r.stdout, 'remove with backslashes', r)

    print(f'{checks} checks, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
