#!/usr/bin/env node
// Offline comparison of explicitly supplied files. Values are redacted by default.
import { open } from "node:fs/promises";
import { MAX_REPORT_BYTES, compareReports, comparisonSummary, validateReport } from "./report.mjs";

const usage = "Usage: node scripts/privacy-evaluation/compare.mjs BEFORE.json AFTER.json [--mode repeat-visit|fresh-identity] [--details]";
async function readReport(path) {
  const file = await open(path, "r");
  try {
    const buffer = Buffer.alloc(MAX_REPORT_BYTES + 1);
    let total = 0;
    while (total < buffer.length) {
      const { bytesRead } = await file.read(buffer, total, buffer.length - total, total);
      if (bytesRead === 0) break;
      total += bytesRead;
    }
    if (total > MAX_REPORT_BYTES) throw new Error("Report exceeds size limit");
    return validateReport(JSON.parse(buffer.subarray(0, total).toString("utf8")));
  } finally { await file.close(); }
}
const args = process.argv.slice(2);
if (args.length === 1 && args[0] === "--help") {
  process.stdout.write(`${usage}\n`);
} else {
  try {
    if (args.length < 2 || args[0].startsWith("--") || args[1].startsWith("--")) throw new Error("Invalid arguments");
    let mode = "repeat-visit";
    let details = false;
    for (let index = 2; index < args.length; index += 1) {
      if (args[index] === "--details") details = true;
      else if (args[index] === "--mode" && index + 1 < args.length) mode = args[++index];
      else throw new Error("Unknown argument");
    }
    const before = await readReport(args[0]);
    const after = await readReport(args[1]);
    const comparison = compareReports(before, after, mode);
    process.stdout.write(`${JSON.stringify(details ? comparison : comparisonSummary(comparison), null, 2)}\n`);
    if (!comparison.compatible) process.exitCode = 2;
  } catch {
    // JSON parser messages can include raw input. Do not print those by default.
    process.stderr.write(`Comparison rejected: invalid arguments, unreadable/oversized files, or invalid report schema.\n${usage}\n`);
    process.exitCode = 2;
  }
}
