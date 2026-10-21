#!/usr/bin/env python3
"""
Tests of the ragc command line tool, run by mock_server.py:

    mock_server.py python3 test_cli.py <path to ragc>
"""

import json
import os
import subprocess
import sys
import tempfile
import urllib.request

RAGC = os.path.abspath(sys.argv[1])
HOST = os.environ['OLLAMA_HOST']
failures = 0
checks = 0


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
    return subprocess.run([RAGC, *args], cwd=cwd, env=clean, capture_output=True, text=True)


def mock(path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    with urllib.request.urlopen(f'http://{HOST}{path}', data) as response:
        return json.loads(response.read())


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)


def main():
    with tempfile.TemporaryDirectory() as tmp:
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
        check('indexed  notes/fruit.md (2 chunks)' in r.stderr, 'index progress', r)
        check('skipped  notes/image.txt: ' in r.stderr and 'binary' in r.stderr, 'binary files are skipped', r)
        check("'/" not in r.stderr, 'paths are relative', r)
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
        check(r.returncode == 0 and r.stdout.startswith('1. notes/fruit.md:1-3  (score '), 'search', r)
        check('   Bananas are yellow' in r.stdout and '\n2. ' in r.stdout, 'search results', r)
        r = ragc('-k', '1', 'search', 'Mars red planet', cwd=tmp)
        check(r.returncode == 0 and 'notes/space.md' in r.stdout and '\n2. ' not in r.stdout, 'top-k', r)
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
        check('\nSources:\n  [1] notes/fruit.md:1-3 (score ' in r.stdout, 'sources', r)
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

        # another embedding model
        r = ragc('-e', 'other-embed', 'search', 'bananas', cwd=tmp)
        check(r.returncode == 1 and "'mock-embed'" in r.stderr and "'other-embed'" in r.stderr and '--db' in r.stderr,
              'model mismatch', r)

        # changes
        write(os.path.join(notes, 'space.md'), '# Space\n\nJupiter is the largest planet.\n')
        os.remove(os.path.join(notes, 'sub', 'cooking.txt'))
        r = ragc('index', 'notes', cwd=tmp)
        check(r.returncode == 0 and '1 indexed, 1 unchanged, 1 removed' in r.stdout, 'incremental index', r)
        check('removed  notes/sub/cooking.txt' in r.stderr, 'removed file', r)

        # a single file, any extension
        write(os.path.join(tmp, 'todo.org'), 'Buy tomatoes and basil.')
        r = ragc('index', 'todo.org', cwd=tmp)
        check(r.returncode == 0 and '1 indexed' in r.stdout and 'indexed  todo.org (1 chunk)' in r.stderr,
              'index a file', r)
        r = ragc('index', 'missing.md', cwd=tmp)
        check(r.returncode == 1 and 'missing.md' in r.stderr, 'index a missing file', r)

        # remove
        r = ragc('remove', 'notes/fruit.md', 'todo.org', cwd=tmp)
        check(r.returncode == 0 and '2 files removed' in r.stdout and 'removed  notes/fruit.md' in r.stderr,
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

    print(f'{checks} checks, {failures} failures')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
