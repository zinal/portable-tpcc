# CE C_ID partition layouts (examples)

Compliant CE partitioning requires `iPartitionPercent = 50`, partition start `% 1000 == 1`, partition size `>= 5000` and `% 1000 == 0`. See [`spec-scalability.md`](../spec-scalability.md) §2.2 and §5.2.

Global configured/active customer counts (`-t` / `-c` or `scale.*` in run-config) describe the **whole database**. Each CE process receives its own partition via `--ce-start-id`, `--ce-part-count`, and `--ce-part-percent`.

## 5,000 customers (single CE)

One CE process covers the full range. Partitioning is optional and usually omitted.

| CE instance | `--ce-start-id` | `--ce-part-count` | `--ce-part-percent` |
| --- | --- | --- | --- |
| `ce1` | (disabled) | (disabled) | (disabled) |

If partitioning is enabled for testing, the only compliant single-partition layout is the full range:

| CE instance | `--ce-start-id` | `--ce-part-count` | `--ce-part-percent` |
| --- | --- | --- | --- |
| `ce1` | 1 | 5000 | 50 |

Two CE instances with partitions are **not** possible at 5k scale because each partition must be at least 5000 customers.

## 50,000 customers (two CE instances)

| CE instance | `--ce-start-id` | `--ce-part-count` | `--ce-part-percent` | `--ce-id-base` |
| --- | --- | --- | --- | --- |
| `ce1` | 1 | 25000 | 50 | 1000 |
| `ce2` | 25001 | 25000 | 50 | 2000 |

UniqueId intervals: `[1000, 1000+N)` and `[2000, 2000+N)` must not overlap (`N = -u`).

## 100,000 customers (two equal CE instances)

| CE instance | `--ce-start-id` | `--ce-part-count` | `--ce-part-percent` | `--ce-id-base` |
| --- | --- | --- | --- | --- |
| `ce1` | 1 | 50000 | 50 | 1000 |
| `ce2` | 50001 | 50000 | 50 | 2000 |

## 100,000 customers (four CE instances)

| CE instance | `--ce-start-id` | `--ce-part-count` | `--ce-part-percent` | `--ce-id-base` |
| --- | --- | --- | --- | --- |
| `ce1` | 1 | 25000 | 50 | 1000 |
| `ce2` | 25001 | 25000 | 50 | 2000 |
| `ce3` | 50001 | 25000 | 50 | 3000 |
| `ce4` | 75001 | 25000 | 50 | 4000 |

Different CE processes may use different partition sizes as long as each satisfies §2.2.
