#!/usr/bin/env python3
"""Build integer-set fixtures from a saved OpenStreetMap XML snapshot.

Usage: python3 make_osm_cow_dataset.py snapshot.osm output-directory
Preserves node IDs. Groups tagged nodes by key=value and way node references
by highway class. Does not mix node, way, and relation identifier namespaces.
Save the source snapshot alongside the generated manifest for reproducibility.
"""
import hashlib
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

source, output = Path(sys.argv[1]), Path(sys.argv[2]).resolve()
output.mkdir(parents=True, exist_ok=True)
groups = {}
for _, element in ET.iterparse(source, events=("end",)):
    if element.tag == "node":
        node = int(element.attrib["id"])
        for tag in element.findall("tag"):
            label = "node/" + tag.attrib["k"] + "=" + tag.attrib["v"]
            groups.setdefault(label, set()).add(node)
        element.clear()
    elif element.tag == "way":
        for tag in element.findall("tag"):
            if tag.attrib["k"] == "highway":
                label = "way-nodes/highway=" + tag.attrib["v"]
                groups.setdefault(label, set()).update(
                    int(nd.attrib["ref"]) for nd in element.findall("nd")
                )
        element.clear()
    elif element.tag == "relation":
        element.clear()

# Retain useful sets, deterministically; tiny groups mostly measure ART overhead.
selected = [
    (label, sorted(values))
    for label, values in sorted(groups.items())
    if len(values) >= 16
]
if not selected:
    raise SystemExit("No groups with at least 16 node IDs")
manifest, metadata = [], []
for i, (label, values) in enumerate(selected):
    path = output / ("set-%03d.txt" % i)
    path.write_text(",".join(map(str, values)) + "\n")
    manifest.append(str(path))
    metadata.append({
        "file": path.name,
        "group": label,
        "count": len(values),
        "min": values[0],
        "max": values[-1],
        "containers": len({v >> 16 for v in values}),
    })
(output / "manifest.txt").write_text("\n".join(manifest) + "\n")
summary = {
    "source": source.name,
    "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "attribution": "© OpenStreetMap contributors; https://www.openstreetmap.org/copyright",
    "sets": metadata,
}
(output / "metadata.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps({
    "sets": len(selected),
    "entries": sum(len(v) for _, v in selected),
    "max_id": max(v[-1] for _, v in selected),
    "manifest": str(output / "manifest.txt"),
}))
