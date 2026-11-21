"""Generate an original AVM1 test game and optional synthetic catalog on disk."""

import argparse
import struct
import zlib
from pathlib import Path


def packed_bits(*fields):
    bits = "".join(f"{number & ((1 << width) - 1):0{width}b}" for number, width in fields)
    bits += "0" * (-len(bits) % 8)
    return int(bits, 2).to_bytes(len(bits) // 8, "big")


def rect(x1, x2, y1, y2):
    width = max(abs(value).bit_length() + 1 for value in (x1, x2, y1, y2))
    return packed_bits((width, 5), *((value, width) for value in (x1, x2, y1, y2)))


def tag(code, data=b""):
    if len(data) < 63:
        return struct.pack("<H", code * 64 + len(data)) + data
    return struct.pack("<HI", code * 64 + 63, len(data)) + data


def action(code, data=b""):
    return bytes([code]) + (struct.pack("<H", len(data)) + data if code >= 128 else b"")


def push(*values):
    result = bytearray()
    for value in values:
        if isinstance(value, str):
            result += b"\x00" + value.encode("utf-8") + b"\x00"
        else:
            result += b"\x07" + struct.pack("<i", value)
    return action(0x96, result)


GET = action(0x1C)
SET = action(0x1D)
GET_MEMBER = action(0x4E)
SET_MEMBER = action(0x4F)
CALL = action(0x52)
POP = action(0x17)
TRACE = action(0x26)
CONCAT = action(0x21)
EQUALS = action(0x49)
INCREMENT = action(0x50)


def variable(name):
    return push(name) + GET


def member(expression, name):
    return expression + push(name) + GET_MEMBER


def method(expression, name, *args):
    return push(*args[::-1], len(args)) + expression + push(name) + CALL


def new_label(name, depth, x, y, value):
    root = variable("_root")
    return (method(root, "createTextField", name, depth, x, y, 360, 25) + POP
            + member(root, name) + push("text") + value + SET_MEMBER)


def movie(tags, width=440, height=220):
    body = (rect(0, width * 20, 0, height * 20) + struct.pack("<HH", 24 * 256, 1)
            + tags + tag(1) + tag(0))
    return b"FWS\x07" + struct.pack("<I", len(body) + 8) + body


def rectangle_shape(character_id, x, y, width, height, color):
    edges = [(0, 1), (0b00101, 5)]
    move_size = max(x, y).bit_length() + 1
    edges += [(move_size, 5), (x, move_size), (y, move_size), (1, 1)]
    for delta, vertical in ((width, 0), (height, 1), (-width, 0), (-height, 1)):
        size = abs(delta).bit_length() + 1
        edges += [(1, 1), (1, 1), (size - 2, 4), (0, 1), (vertical, 1), (delta, size)]
    edges.append((0, 6))
    return tag(32, struct.pack("<H", character_id) + rect(x, x + width, y, y + height)
               + b"\x01\x00" + bytes((*color, 255)) + b"\x00\x10" + packed_bits(*edges))


def make_game():
    data = member(variable("fffStorage"), "data")
    opens = member(data, "opens")
    default = data + push("opens", 0) + SET_MEMBER
    code = push("fffStorage") + method(variable("SharedObject"), "getLocal", "portable") + SET
    code += opens + action(0x9D, struct.pack("<h", len(default))) + default
    code += data + push("opens") + opens + INCREMENT + SET_MEMBER
    code += method(variable("fffStorage"), "flush") + POP
    code += push("FFF portable opens=") + opens + CONCAT + TRACE
    code += new_label("titleLabel", 4, 28, 24, push("FlashFasaFiso / Mini hedef"))
    code += new_label("openLabel", 5, 28, 48, push("Kayitli acilis: ") + opens + CONCAT)
    code += new_label("hitLabel", 6, 28, 163, push("Hedefe tiklayin."))
    code += push("hits", 0) + SET
    code += method(variable("_root"), "createEmptyMovieClip", "externalAsset", 7) + POP
    code += method(member(variable("_root"), "externalAsset"), "loadMovie", "assets/marker.swf") + POP
    button_action = push("hits") + variable("hits") + INCREMENT + SET
    button_action += member(variable("_root"), "hitLabel") + push("text", "Isabet: ") + variable("hits") + CONCAT + SET_MEMBER
    button_action += push("FFF hits=") + variable("hits") + CONCAT + TRACE
    shape = rectangle_shape(1, 560, 1600, 3120, 1350, (98, 213, 200))
    matrix = b"\x00"
    button = tag(7, struct.pack("<HBHH", 2, 0x0F, 1, 1) + matrix + b"\x00" + button_action + b"\x00")
    place_button = tag(4, struct.pack("<HH", 2, 1) + matrix)
    background = tag(9, bytes((231, 241, 241)))
    return movie(background + shape + button + place_button + tag(12, code + b"\x00"))


def cover_png():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    width, height = 240, 140
    rows = bytearray()
    for y in range(height):
        rows += b"\0"
        for x in range(width):
            center = (x - 119) ** 2 + (y - 69) ** 2
            rows += bytes((96, 220, 203) if center < 43 ** 2 else
                          (27 + x // 12, 66 + y // 6, 86 + x // 7))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def write(path, bytes_):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(bytes_)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--count", type=int, default=4)
    parser.add_argument("--compress", action="store_true", help="Write the main SWFs with CWS compression")
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be positive")
    games = args.root / "games"
    game = games / "İz ve Işık 雪"
    main_swf = make_game()
    if args.compress:
        main_swf = b"C" + main_swf[1:8] + zlib.compress(main_swf[8:])
    marker = movie(tag(12, push("FFF relative asset loaded") + TRACE + b"\x00"), 16, 16)
    write(game / "hedef & kar.swf", main_swf)
    write(game / "assets" / "marker.swf", marker)
    write(game / "game.ini", ("title=İz ve Işık 雪\ngenre=Mini oyun\n"
                              "description=Hedefe tıklayın. Açılış sayacı Ruffle kaydından yüklenir.\n").encode("utf-8"))
    write(game / "cover.png", cover_png())
    for i in range(1, args.count):
        name = "Çoklu SWF" if i == 1 else f"Oyun {i:04d}"
        write(games / name / "başla.swf", main_swf)
        if i == 1:
            write(games / name / "giriş 2.swf", main_swf)
            write(games / name / "cover.png", b"invalid image fixture")


if __name__ == "__main__":
    main()
