from pathlib import Path
import cairosvg

root = Path(__file__).resolve().parents[1]
src = root / "src" / "resource" / "125A_Logo_Master_FINAL.svg"
dst = root / "src" / "resource" / "125A_Logo.png"

if not src.exists():
    raise SystemExit(f"Branding master missing: {src}")

cairosvg.svg2png(
    bytestring=src.read_bytes(),
    write_to=str(dst),
    output_width=128,
    output_height=64,
)
print(f"Rendered {dst} from authoritative branding master {src}")
