#!/usr/bin/python3
#
# Scrape model pricing from Mistral AI documentation and Entrim AI model catalog.
#

import argparse
import csv
from html.parser import HTMLParser
import json
import os
import re
import sys
import urllib.request


class MistralTableParser(HTMLParser):
	def __init__(self):
		super().__init__()
		self.tables = []
		self._in_table = False
		self._in_cell = False
		self._cell_text = []
		self._cell_href = None
		self._row = []
		self._current_table = []

	def handle_starttag(self, tag, attrs):
		attrs_dict = dict(attrs)
		if tag == "table":
			self._in_table = True
			self._current_table = []
		elif self._in_table and tag in ("td", "th"):
			self._in_cell = True
			self._cell_text = []
			self._cell_href = None
		elif self._in_cell and tag == "a":
			if "href" in attrs_dict and not self._cell_href:
				self._cell_href = attrs_dict["href"]

	def handle_endtag(self, tag):
		if tag == "table":
			if self._current_table:
				self.tables.append(self._current_table)
			self._in_table = False
		elif self._in_table and tag == "tr":
			if self._row:
				self._current_table.append(self._row)
				self._row = []
		elif self._in_table and tag in ("td", "th"):
			text = "".join(self._cell_text).strip()
			self._row.append((text, self._cell_href))
			self._in_cell = False

	def handle_data(self, data):
		if self._in_cell:
			self._cell_text.append(data)


def fetch_url(url, timeout=15):
	req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (X11; Linux x86_64)"})
	with urllib.request.urlopen(req, timeout=timeout) as resp:
		return resp.read().decode("utf-8", errors="replace")


def clean_price(val):
	if not val:
		return ""
	val = val.strip()
	if val.lower() == "free":
		return "Free"
	if val in ("—", "-", "N/A", "n/a"):
		return "—"
	if "/" in val:
		val = val.split("/")[0].strip()
	if val and not val.startswith("$") and val[0].isdigit():
		val = "$" + val
	return val


def clean_model_name(name):
	name = name.replace("\u2197", "").replace("↗", "").strip()
	return name


def scrape_mistral(url, timeout=15):
	html = fetch_url(url, timeout)
	parser = MistralTableParser()
	parser.feed(html)

	rows = []
	for table in parser.tables:
		if not table:
			continue
		header_row = [cell[0].lower() for cell in table[0]]
		if "model" not in header_row or "input" not in header_row:
			continue

		for row in table[1:]:
			if len(row) < 2:
				continue
			name_cell, href = row[0]
			model_name = clean_model_name(name_cell)
			if not model_name:
				continue

			model_id = href.strip("/").split("/")[-1] if href else model_name

			input_raw = row[1][0] if len(row) > 1 else ""
			cached_raw = row[2][0] if len(row) > 2 else ""
			output_raw = row[3][0] if len(row) > 3 else ""

			unit = "1M tokens"
			for raw in (input_raw, cached_raw, output_raw):
				if "/" in raw:
					unit = raw.split("/")[-1].strip()
					break

			rows.append({
				"provider": "Mistral",
				"model_name": model_name,
				"model_id": model_id,
				"input_cost": clean_price(input_raw),
				"cached_cost": clean_price(cached_raw),
				"output_cost": clean_price(output_raw),
				"unit": unit,
			})
	return rows


def scrape_entrim(url, timeout=15):
	html = fetch_url(url, timeout)

	# Extract Next.js streamed chunks: self.__next_f.push([1, "..."])
	chunks = []
	for m in re.finditer(r"self\.__next_f\.push\(\[1,\s*(\".*?\")\]\)", html, re.DOTALL):
		try:
			chunks.append(json.loads(m.group(1)))
		except Exception:
			pass
	full_payload = "".join(chunks)

	marker = "\"models\":["
	idx = full_payload.find(marker)
	if idx == -1:
		return []

	decoder = json.JSONDecoder()
	models_list, _ = decoder.raw_decode(full_payload[idx + len("\"models\":"):])

	rows = []
	for m in models_list:
		price = m.get("price", {})
		inp = clean_price(str(price.get("input", "")))
		outp = clean_price(str(price.get("output", "")))
		cached = clean_price(str(price.get("cacheRead", "")))

		rows.append({
			"provider": "Entrim",
			"model_name": m.get("name") or m.get("id", ""),
			"model_id": m.get("apiModelName") or m.get("id", ""),
			"input_cost": inp,
			"cached_cost": cached,
			"output_cost": outp,
			"unit": "1M tokens",
		})
	return rows


def main():
	parser = argparse.ArgumentParser(description="Scrape model names and token pricing from Mistral and Entrim into CSV.")
	parser.add_argument("-o", "--output", default="model_pricing.csv", help="Output CSV file path (default: model_pricing.csv, or '-' for stdout)")
	parser.add_argument("--mistral-url", default="https://docs.mistral.ai/inference/pricing", help="Mistral pricing documentation URL")
	parser.add_argument("--entrim-url", default="https://entrim.ai/ai-models/", help="Entrim AI models catalog URL")
	parser.add_argument("--timeout", type=int, default=15, help="HTTP request timeout in seconds (default: 15)")
	args = parser.parse_args()

	rows = []

	print("Scraping Mistral pricing...", file=sys.stderr)
	try:
		mistral_rows = scrape_mistral(args.mistral_url, args.timeout)
		print(f"  Found {len(mistral_rows)} Mistral models", file=sys.stderr)
		rows.extend(mistral_rows)
	except Exception as e:
		print(f"  Warning: failed to scrape Mistral: {e}", file=sys.stderr)

	print("Scraping Entrim pricing...", file=sys.stderr)
	try:
		entrim_rows = scrape_entrim(args.entrim_url, args.timeout)
		print(f"  Found {len(entrim_rows)} Entrim models", file=sys.stderr)
		rows.extend(entrim_rows)
	except Exception as e:
		print(f"  Warning: failed to scrape Entrim: {e}", file=sys.stderr)

	fieldnames = ["provider", "model_name", "model_id", "input_cost", "cached_cost", "output_cost", "unit"]

	if args.output == "-":
		writer = csv.DictWriter(sys.stdout, fieldnames=fieldnames)
		writer.writeheader()
		writer.writerows(rows)
	else:
		with open(args.output, "w", newline="", encoding="utf-8") as f:
			writer = csv.DictWriter(f, fieldnames=fieldnames)
			writer.writeheader()
			writer.writerows(rows)
		print(f"Wrote {len(rows)} entries to {args.output}", file=sys.stderr)


if __name__ == "__main__":
	main()
