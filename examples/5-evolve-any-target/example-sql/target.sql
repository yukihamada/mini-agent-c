-- Top 10 countries by total order amount (2025 only).
-- This is a correctness target: result set must stay identical after optimization.
SELECT country, SUM(amount) AS total
FROM users, orders
WHERE users.id = orders.user_id
  AND orders.created_at LIKE '2025%'
GROUP BY country
ORDER BY total DESC
LIMIT 10;
