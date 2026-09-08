# OpenStreetMap node-ID benchmark sample

A small Monaco extract prepared with `benchmarks/make_osm_cow_dataset.py`.
Each text file is a set of original node IDs: either nodes sharing a tag, or
nodes referenced by ways sharing a highway classification. Way/relation IDs
are never inserted. Groups with fewer than 16 IDs were omitted.

This snapshot contains 46 sets, 5,612 total entries across sets, and a maximum
ID of 14,140,358,910. It crosses the unsigned 32-bit boundary naturally. It is a
small, sparse sample, not a representative distribution of all OSM data or of
all 64-bit workloads. The synthetic broad64 cases exercise much wider ART keys.

`metadata.json` records each group's meaning, size, container count, source
URL, and the SHA-256 of the source XML snapshot. The checked-in integer sets
are the reproducible benchmark inputs; fetching the live URL later may return
different data. The XML itself is not included.

© OpenStreetMap contributors. Data is available under the Open Database License
(ODbL): https://www.openstreetmap.org/copyright and
https://opendatacommons.org/licenses/odbl/1-0/.
