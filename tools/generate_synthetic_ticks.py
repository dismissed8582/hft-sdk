#!/usr/bin/env python3
"""Generate a deterministic synthetic LOB stream with structural order profiles."""

from __future__ import annotations

import argparse
import heapq
import math
import random
from dataclasses import dataclass
from pathlib import Path

NS_PER_MS = 1_000_000


@dataclass
class Order:
    side: str
    price: int
    created_ns: int
    modifications: int = 0


def sigmoid(value: float) -> float:
    if value >= 0.0:
        z = math.exp(-value)
        return 1.0 / (1.0 + z)
    z = math.exp(value)
    return z / (1.0 + z)


def generate(duration_seconds: int, seed: int) -> list[tuple]:
    rng = random.Random(seed)
    steps = duration_seconds * 1000
    events: list[tuple] = []
    scheduled: list[tuple[int, int, str, int]] = []
    orders: dict[int, Order] = {}
    active_ids: list[int] = []
    active_index: dict[int, int] = {}
    next_order_id = 1
    next_trade_id = 50_000_000
    sequence = 0
    latent = 0.0
    mid = 10_000.0

    for step in range(steps):
        timestamp_ns = step * NS_PER_MS
        latent = 0.9985 * latent + rng.gauss(0.0, 0.018)
        mid += latent * 0.02 + rng.gauss(0.0, 0.02)
        mid_ticks = max(10, int(round(mid)))
        best_bid = mid_ticks - 1
        best_ask = mid_ticks + 1
        ask_probability = sigmoid(2.4 * latent)

        while scheduled and scheduled[0][0] <= timestamp_ns:
            due_ns, order_seq, kind, order_id = heapq.heappop(scheduled)
            order = orders.get(order_id)
            if order is None:
                continue
            if kind == "modify":
                order.modifications += 1
                events.append(
                    (
                        due_ns,
                        order_seq,
                        "MODIFY",
                        order.side,
                        order_id,
                        order.price,
                        rng.randint(1, 100),
                        order.modifications,
                        best_bid,
                        best_ask,
                    )
                )
            else:
                events.append(
                    (
                        due_ns,
                        order_seq,
                        "CANCEL",
                        order.side,
                        order_id,
                        order.price,
                        rng.randint(1, 100),
                        order.modifications,
                        best_bid,
                        best_ask,
                    )
                )
                orders.pop(order_id, None)
                index = active_index.pop(order_id, None)
                if index is not None:
                    last_id = active_ids.pop()
                    if index < len(active_ids):
                        active_ids[index] = last_id
                        active_index[last_id] = index

        if rng.random() < 0.20:
            profile = rng.choices(
                ("normal", "short", "flicker"), weights=(0.55, 0.25, 0.20), k=1
            )[0]
            if profile == "normal":
                lifetime_ms = rng.randint(500, 8_000)
                modification_gap_ms = rng.randint(150, 900)
            elif profile == "short":
                lifetime_ms = rng.randint(10, 90)
                modification_gap_ms = rng.randint(5, 30)
            else:
                lifetime_ms = rng.randint(500, 5_000)
                modification_gap_ms = 1

            noise = profile != "normal" and rng.random() < 0.70
            if noise:
                side = "ASK" if rng.random() < 0.5 else "BID"
            else:
                side = "ASK" if rng.random() < ask_probability else "BID"

            offset = rng.randint(1, 3)
            price = best_bid - offset if side == "BID" else best_ask + offset
            order_id = next_order_id
            next_order_id += 1
            order = Order(side, price, timestamp_ns)
            orders[order_id] = order
            active_index[order_id] = len(active_ids)
            active_ids.append(order_id)
            sequence += 1
            events.append(
                (
                    timestamp_ns,
                    sequence,
                    "NEW",
                    side,
                    order_id,
                    price,
                    rng.randint(1, 100),
                    0,
                    best_bid,
                    best_ask,
                )
            )

            modification_count = 0
            if profile == "normal":
                modification_count = rng.randint(0, 3)
            elif profile == "short":
                modification_count = rng.randint(0, 2)
            else:
                modification_count = rng.randint(20, 80)

            elapsed_ms = modification_gap_ms
            for _ in range(modification_count):
                if elapsed_ms >= lifetime_ms:
                    break
                sequence += 1
                heapq.heappush(
                    scheduled,
                    (
                        timestamp_ns + elapsed_ms * NS_PER_MS,
                        sequence,
                        "modify",
                        order_id,
                    ),
                )
                if profile == "flicker":
                    elapsed_ms += rng.randint(0, 1)
                else:
                    elapsed_ms += rng.randint(
                        modification_gap_ms, modification_gap_ms * 2 + 1
                    )

            sequence += 1
            heapq.heappush(
                scheduled,
                (
                    timestamp_ns + lifetime_ms * NS_PER_MS,
                    sequence,
                    "cancel",
                    order_id,
                ),
            )

        if rng.random() < 0.08:
            side = "BUY" if rng.random() < sigmoid(2.8 * latent) else "SELL"
            price = best_ask if side == "BUY" else best_bid
            sequence += 1
            trade_order_id = next_trade_id
            if active_ids and rng.random() < 0.85:
                trade_order_id = rng.choice(active_ids)
            events.append(
                (
                    timestamp_ns,
                    sequence,
                    "TRADE",
                    side,
                    trade_order_id,
                    price,
                    rng.randint(1, 50),
                    0,
                    best_bid,
                    best_ask,
                )
            )
            next_trade_id += 1

    events.sort(key=lambda event: (event[0], event[1]))
    return events


def write_csv(events: list[tuple], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as output:
        output.write(
            "timestamp_ns,event_type,side,order_id,price_ticks,quantity,"
            "modifications,bid_price_ticks,ask_price_ticks\n"
        )
        for event in events:
            output.write(
                f"{event[0]},{event[2]},{event[3]},{event[4]},{event[5]},"
                f"{event[6]},{event[7]},{event[8]},{event[9]}\n"
            )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=int, default=180)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--output", type=Path, default=Path("data/sample_ticks.csv"))
    args = parser.parse_args()

    events = generate(args.duration, args.seed)
    write_csv(events, args.output)
    counts: dict[str, int] = {}
    for event in events:
        counts[event[2]] = counts.get(event[2], 0) + 1
    print(
        f"wrote {len(events):,} events to {args.output} "
        f"({args.duration}s, seed={args.seed}) {counts}"
    )


if __name__ == "__main__":
    main()
