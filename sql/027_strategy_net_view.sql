-- 027: Net-of-fee per-strategy P&L view.
--
-- vp.fills.fee_usd carries the modeled taker fee on entry/exit fills
-- (maker fills and settlement redemptions are correctly 0). Raw sums of
-- fill value therefore overstate performance; this view nets proceeds,
-- cost and fees per instance/day and adds settlement win/loss counts so
-- dashboards report the same numbers as scripts/accounting.py.
--
-- Idempotent: CREATE OR REPLACE.

CREATE OR REPLACE VIEW vp.v_strategy_net_pnl AS
SELECT o.instance,
       (f.ts AT TIME ZONE 'UTC')::date AS day,
       count(*) FILTER (WHERE o.direction = 'buy')          AS buy_fills,
       count(*) FILTER (WHERE o.direction = 'sell')         AS sell_fills,
       count(*) FILTER (WHERE o.source = 'settle'
                          AND f.price > 0.5)                AS settles_won,
       count(*) FILTER (WHERE o.source = 'settle'
                          AND f.price <= 0.5)               AS settles_lost,
       round(sum(f.price * f.size_shares)
             FILTER (WHERE o.direction = 'sell')::numeric, 4)  AS proceeds_usd,
       round(sum(f.price * f.size_shares)
             FILTER (WHERE o.direction = 'buy')::numeric, 4)   AS cost_usd,
       round(sum(f.fee_usd)::numeric, 4)                        AS fees_usd,
       round((sum(f.price * f.size_shares) FILTER (WHERE o.direction = 'sell')
            - sum(f.price * f.size_shares) FILTER (WHERE o.direction = 'buy')
            - sum(f.fee_usd))::numeric, 4)                      AS net_pnl_usd
FROM vp.orders o
JOIN vp.fills f ON f.client_order_id = o.client_order_id
WHERE o.instance IS NOT NULL AND o.instance <> ''
GROUP BY o.instance, (f.ts AT TIME ZONE 'UTC')::date;
