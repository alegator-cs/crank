# Calibrated process CPU follow-up

The original broad run used a CPU resource limit that caused stale process-CPU clock readings in this runtime. These separate observations omit `RLIMIT_CPU`; the external wall timeout, address-space cap, common CPU affinity, fixture inputs, and compiled benchmark binary remain in place. The [clock calibration](clock_resolution_probe.json) compares process and thread clocks with each resource setting.

Six representative cases were run in two explicitly recorded batches, with native and assisted modes 2/4/8 and three fresh processes each: **72 completed, correct observations**, with source/binary integrity checks passing in both batches. These observations do not replace the broad wall-time corpus.

All values below are microseconds. Wall return is the median of process medians of five warm batch means. Complete CPU/query is the median of process totals covering all warm batches and their single final helper drain, divided by warm query count. It includes background work that continues after a query returns. The separate diagnostic tail is outside the query latency.

| Case | Mode | Wall return | Complete CPU/query | Diagnostic return-to-idle |
|---|---|---:|---:|---:|
| greedy_boundary_8192_accept | assist2 | 3,095.703 | 3,199.255 | 0.130 |
| greedy_boundary_8192_accept | assist4 | 3,201.322 | 3,256.675 | 0.180 |
| greedy_boundary_8192_accept | assist8 | 3,041.551 | 3,336.979 | 0.160 |
| greedy_boundary_8192_accept | native | 2,754.479 | 2,818.988 | 0.030 |
| independent_branches_late_order_reject_4096 | assist2 | 11.663 | 16.273 | 11.968 |
| independent_branches_late_order_reject_4096 | assist4 | 20.158 | 36.378 | 8.953 |
| independent_branches_late_order_reject_4096 | assist8 | 43.746 | 79.640 | 12.128 |
| independent_branches_late_order_reject_4096 | native | 5.667 | 5.735 | 0.030 |
| json_rows_8192_valid | assist2 | 12,456.221 | 12,428.866 | 0.240 |
| json_rows_8192_valid | assist4 | 12,811.375 | 13,108.177 | 0.370 |
| json_rows_8192_valid | assist8 | 13,054.161 | 13,137.765 | 0.140 |
| json_rows_8192_valid | native | 11,970.939 | 12,210.344 | 0.020 |
| mixed_prefix_late_marker_reject_4096 | assist2 | 129.370 | 217.956 | 0.030 |
| mixed_prefix_late_marker_reject_4096 | assist4 | 24.694 | 49.386 | 0.030 |
| mixed_prefix_late_marker_reject_4096 | assist8 | 45.509 | 98.889 | 12.128 |
| mixed_prefix_late_marker_reject_4096 | native | 52,277.187 | 52,545.338 | 0.141 |
| split_frontier_1024_internal_reject | assist2 | 14.786 | 23.500 | 0.050 |
| split_frontier_1024_internal_reject | assist4 | 17.753 | 27.619 | 9.043 |
| split_frontier_1024_internal_reject | assist8 | 40.175 | 83.779 | 9.814 |
| split_frontier_1024_internal_reject | native | 86,806.624 | 85,219.508 | 0.030 |
| independent_branches_last_accept_4096 | assist2 | 6,791.101 | 7,057.009 | 0.160 |
| independent_branches_last_accept_4096 | assist4 | 7,196.771 | 7,702.240 | 0.250 |
| independent_branches_last_accept_4096 | assist8 | 6,621.936 | 6,915.292 | 0.201 |
| independent_branches_last_accept_4096 | native | 92,271.385 | 92,951.173 | 0.150 |

The late-marker and split-frontier rejections save both latency and total CPU. The accepted independent-branch case also benefits: runtime node exclusions prevent native exploration of absent branches. Helpers add work to the already-cheap late-order rejection and to surviving greedy/JSON cases. Increasing worker count does not reliably improve either cost.

Evidence: [five-case environment](cpu_followup/environment.json), [five-case raw observations](cpu_followup/raw.jsonl), [accepted-branch environment](cpu_accept_followup/environment.json), [accepted-branch raw observations](cpu_accept_followup/raw.jsonl).
