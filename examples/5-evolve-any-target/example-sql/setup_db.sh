#!/bin/bash
# Create a small sqlite DB for the evolution demo.
set -e
DB="/tmp/evolve_sql.db"
rm -f "$DB"
sqlite3 "$DB" <<'SQL'
CREATE TABLE users (
    id INTEGER PRIMARY KEY,
    name TEXT,
    email TEXT,
    country TEXT,
    age INTEGER,
    signup_date TEXT
);
CREATE TABLE orders (
    id INTEGER PRIMARY KEY,
    user_id INTEGER,
    amount REAL,
    created_at TEXT
);
SQL

# Generate 50k users + 500k orders — big enough that a full-table-scan JOIN
# is clearly slower than an indexed one.
python3 - <<'PY'
import sqlite3, random, string
random.seed(42)  # deterministic so reference matches across runs
c = sqlite3.connect('/tmp/evolve_sql.db')
cur = c.cursor()
cur.execute('PRAGMA journal_mode=WAL')
countries = ['JP','US','GB','DE','FR','BR','IN','CN','KR','SG']
users = []
for i in range(1, 50001):
    name = ''.join(random.choices(string.ascii_lowercase, k=8))
    users.append((i, name, f'{name}@example.com', random.choice(countries), random.randint(18, 80),
                  f'2024-{random.randint(1,12):02d}-{random.randint(1,28):02d}'))
cur.executemany('INSERT INTO users VALUES (?,?,?,?,?,?)', users)
orders = []
for i in range(1, 500001):
    orders.append((i, random.randint(1, 50000), round(random.uniform(10, 5000), 2),
                   f'202{random.randint(4,5)}-{random.randint(1,12):02d}-{random.randint(1,28):02d}'))
cur.executemany('INSERT INTO orders VALUES (?,?,?,?)', orders)
c.commit()
c.close()
PY

echo "Created $DB with 50k users + 500k orders"
sqlite3 "$DB" "SELECT 'users:', COUNT(*) FROM users; SELECT 'orders:', COUNT(*) FROM orders;"
