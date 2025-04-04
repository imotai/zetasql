#
# Copyright 2019 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# The subquery is unnecessary. Linear pipe flow works without it.
# Selecting n2.n_name and aliasing it is unnecessary since the original
# columns (and their table aliases) are still present after using EXTEND
# to compute expressions.
FROM
  part,
  supplier,
  lineitem,
  orders,
  customer,
  nation n1,
  nation n2,
  region
|> WHERE
    p_partkey = l_partkey
    AND s_suppkey = l_suppkey
    AND l_orderkey = o_orderkey
    AND o_custkey = c_custkey
    AND c_nationkey = n1.n_nationkey
    AND n1.n_regionkey = r_regionkey
    AND r_name = 'AMERICA'
    AND s_nationkey = n2.n_nationkey
    AND o_orderdate BETWEEN date '1993-01-01' AND date '1997-12-31'
    AND p_type = 'STANDARD POLISHED TIN'
|> EXTEND
    EXTRACT(year FROM o_orderdate) AS o_year,
    l_extendedprice * (1 - l_discount) AS volume
|> AGGREGATE
    sum(CASE WHEN n2.n_name = 'PERU' THEN volume ELSE 0 END) / sum(volume) AS mkt_share
   GROUP BY
    o_year ASC;
