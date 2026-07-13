# Development tools

## Release packaging

After `idf.py build` and `idf.py merge-bin`, run:

```sh
./tools/package_release.sh
```

The script creates the cleaned source tree, firmware files, zip archives and
SHA-256 manifests under `dist/v1.1/`.

## Font generation

`gen_ui_font_sizes.py` regenerates the 8/12 px bitmap header. The original OFL
font inputs are intentionally not redistributed in this repository; provide
your own licensed copies explicitly:

```sh
python tools/gen_ui_font_sizes.py \
  --font /path/to/jiangcheng-xiehei-200w.ttf \
  --unifont /path/to/unifont.hex.gz
```

The generated data remain under SIL Open Font License 1.1. See
`docs/licenses/OFL-1.1.txt`.
