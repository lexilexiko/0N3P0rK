#!/usr/bin/env python3
"""Download a regional raster tile pack for the 0N3P0rK GPS map."""

import argparse
import ctypes
import math
import os
import sys
import time
import urllib.error
import urllib.request

WEB_MERCATOR_LIMIT = 85.05112878
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
IS_SD_MAPS_DIR = (
    os.path.basename(SCRIPT_DIR).lower() == "maps"
    and os.path.basename(os.path.dirname(SCRIPT_DIR)).lower() == "0n3p0rk"
)
LOCAL_OUTPUT = os.path.join(SCRIPT_DIR, "gps-map-output")
MAX_TILE_BYTES = 5 * 1024 * 1024


def removable_drives():
    if os.name != "nt":
        return []
    bitmask = ctypes.windll.kernel32.GetLogicalDrives()
    drives = []
    for index in range(26):
        if bitmask & (1 << index):
            root = "{}:\\".format(chr(ord("A") + index))
            if ctypes.windll.kernel32.GetDriveTypeW(root) == 2:
                drives.append(root)
    return drives


def choose_output():
    if IS_SD_MAPS_DIR:
        return os.path.dirname(os.path.dirname(SCRIPT_DIR))

    drives = removable_drives()
    if not drives:
        print("No removable drive detected; using local output folder.")
        return LOCAL_OUTPUT

    print("Choose where to save the GPS map tiles:")
    for index, drive in enumerate(drives, 1):
        print("  {}) {} (SD/removable drive)".format(index, drive))
    print("  0) Local folder: {}".format(LOCAL_OUTPUT))
    while True:
        choice = input("Destination: ").strip()
        if choice == "0":
            return LOCAL_OUTPUT
        try:
            selected = int(choice)
        except ValueError:
            selected = 0
        if 1 <= selected <= len(drives):
            return drives[selected - 1]
        print("Enter one of the listed numbers.")


def tile_y(latitude, zoom):
    latitude = max(-WEB_MERCATOR_LIMIT, min(WEB_MERCATOR_LIMIT, latitude))
    radians = math.radians(latitude)
    count = 1 << zoom
    y = int((1.0 - math.asinh(math.tan(radians)) / math.pi) * 0.5 * count)
    return max(0, min(count - 1, y))


def tiles_for_radius(latitude, longitude, radius_km, zoom):
    count = 1 << zoom
    latitude_delta = radius_km / 110.574
    latitude_min = max(-WEB_MERCATOR_LIMIT, latitude - latitude_delta)
    latitude_max = min(WEB_MERCATOR_LIMIT, latitude + latitude_delta)
    cosine = max(0.01, math.cos(math.radians(latitude)))
    longitude_delta = min(180.0, radius_km / (111.320 * cosine))
    x_center = (longitude + 180.0) / 360.0 * count
    x_delta = longitude_delta / 360.0 * count
    x_min = math.floor(x_center - x_delta)
    x_max = math.floor(x_center + x_delta)
    y_min = tile_y(latitude_max, zoom)
    y_max = tile_y(latitude_min, zoom)

    return sorted({
        (x % count, y)
        for x in range(x_min, x_max + 1)
        for y in range(y_min, y_max + 1)
        if 0 <= y < count
    })


def download_tile(url, target, delay):
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": "0N3P0rK-GPS-Map-Downloader/1.0",
            "Accept": "image/png,image/jpeg",
        },
    )
    last_error = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                data = response.read(MAX_TILE_BYTES + 1)
            if len(data) > MAX_TILE_BYTES:
                raise ValueError("tile exceeds 5 MiB")
            if data.startswith(b"\x89PNG\r\n\x1a\n"):
                extension = ".png"
            elif data.startswith(b"\xff\xd8\xff"):
                extension = ".jpg"
            else:
                raise ValueError("server response is not a PNG or JPEG tile")

            path = target + extension
            os.makedirs(os.path.dirname(path), exist_ok=True)
            temporary = path + ".part"
            try:
                with open(temporary, "wb") as output:
                    output.write(data)
                os.replace(temporary, path)
            finally:
                if os.path.exists(temporary):
                    os.remove(temporary)
            time.sleep(delay)
            return path
        except (urllib.error.URLError, TimeoutError, ValueError) as error:
            last_error = error
            if attempt < 2:
                time.sleep(delay * (attempt + 1))
    raise RuntimeError("could not download {}: {}".format(url, last_error))


def parse_args():
    parser = argparse.ArgumentParser(
        description="Build an offline GPS map tile pack for the SD card."
    )
    parser.add_argument("--lat", type=float, required=True, help="center latitude")
    parser.add_argument("--lon", type=float, required=True, help="center longitude")
    parser.add_argument(
        "--radius-km", type=float, default=5.0,
        help="radius around the center (default: 5 km)",
    )
    parser.add_argument(
        "--min-zoom", type=int, default=16,
        help="first zoom level, 2-18 (default: 16)",
    )
    parser.add_argument(
        "--max-zoom", type=int, default=16,
        help="last zoom level, 2-18 (default: 16)",
    )
    parser.add_argument(
        "--url-template", required=True,
        help="permitted tile URL containing {z}, {x}, and {y}",
    )
    parser.add_argument(
        "--attribution", required=True,
        help="attribution text required by the tile provider",
    )
    parser.add_argument(
        "--folder", choices=("dark", "osm", "voyager", "topo"), default="dark",
        help="style folder name used under tiles/ (default: dark)",
    )
    parser.add_argument(
        "--output",
        help="destination root (tiles go under <root>\\0N3P0rK\\maps; default: choose a removable drive)",
    )
    parser.add_argument(
        "--delay", type=float, default=1.0,
        help="minimum seconds between successful requests (default: 1)",
    )
    parser.add_argument(
        "--max-tiles", type=int, default=600,
        help="abort if the map needs more tiles (default: 600)",
    )
    args = parser.parse_args()

    if not math.isfinite(args.lat) or not -90.0 <= args.lat <= 90.0:
        parser.error("--lat must be between -90 and 90")
    if not math.isfinite(args.lon) or not -180.0 <= args.lon <= 180.0:
        parser.error("--lon must be between -180 and 180")
    if not math.isfinite(args.radius_km) or not 0 < args.radius_km <= 500:
        parser.error("--radius-km must be greater than 0 and at most 500")
    if not 2 <= args.min_zoom <= args.max_zoom <= 18:
        parser.error("zoom levels must satisfy 2 <= min-zoom <= max-zoom <= 18")
    if not math.isfinite(args.delay) or args.delay < 0:
        parser.error("--delay must be zero or greater")
    if args.max_tiles < 1:
        parser.error("--max-tiles must be at least 1")
    if not all(token in args.url_template for token in ("{z}", "{x}", "{y}")):
        parser.error("--url-template must contain {z}, {x}, and {y}")
    if not args.attribution.strip():
        parser.error("--attribution cannot be empty")
    return args


def main():
    args = parse_args()
    output_root = args.output if args.output else choose_output()
    tile_list = [
        (zoom, x, y)
        for zoom in range(args.min_zoom, args.max_zoom + 1)
        for x, y in tiles_for_radius(args.lat, args.lon, args.radius_km, zoom)
    ]
    if len(tile_list) > args.max_tiles:
        print(
            "Refusing to download {} tiles (limit {}). Reduce radius/zooms or "
            "raise --max-tiles.".format(len(tile_list), args.max_tiles),
            file=sys.stderr,
        )
        return 2

    maps_dir = os.path.join(output_root, "0N3P0rK", "maps")
    tiles_dir = os.path.join(maps_dir, "tiles", args.folder)
    os.makedirs(tiles_dir, exist_ok=True)
    attribution_path = os.path.join(maps_dir, "ATTRIBUTION.txt")
    with open(attribution_path, "w", encoding="utf-8", newline="\n") as attribution:
        attribution.write(args.attribution.strip() + "\n")

    print("Tiles planned: {}".format(len(tile_list)))
    print("Style folder: {}".format(args.folder))
    print("Output map folder: {}".format(maps_dir))
    for index, (zoom, x, y) in enumerate(tile_list, 1):
        target = os.path.join(tiles_dir, str(zoom), str(x), str(y))
        if os.path.exists(target + ".jpg") or os.path.exists(target + ".png"):
            print("[{}/{}] cached {}/{}/{}".format(index, len(tile_list), zoom, x, y))
            continue
        url = args.url_template.format(z=zoom, x=x, y=y)
        try:
            path = download_tile(url, target, args.delay)
        except (RuntimeError, OSError) as error:
            print("ERROR: {}".format(error), file=sys.stderr)
            print("Downloaded tiles are kept; rerun to resume.", file=sys.stderr)
            return 1
        print("[{}/{}] saved {}/{}/{} ({})".format(
            index, len(tile_list), zoom, x, y, os.path.basename(path)
        ))

    if IS_SD_MAPS_DIR and not args.output:
        print("Done. Tiles saved directly to the SD card.")
    else:
        print("Done. Map tiles saved to the selected destination.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
