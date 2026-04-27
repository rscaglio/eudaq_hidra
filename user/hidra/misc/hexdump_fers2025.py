#!/usr/bin/env python3
import sys

def dump_file(fname):
    with open(fname, "rb") as f:
        data = f.read()

    size = len(data)
    i = 0

    def print_bytes(start, count, newline_every=None):
        idx = start
        printed = 0

        # print index at beginning of first line
        LineHeader = 'FH' if count == 25 else 'EH' if count == 27 else 'BP'
        print(f"{LineHeader}_{idx:09d}: ", end="")

        for j in range(count):
            if idx >= size:
                return idx

            print(f"{data[idx]:02X}", end=" ")
            idx += 1
            printed += 1

            if newline_every and printed % newline_every == 0:
                print()
                if idx < size:
                    print(f"{LineHeader}_{idx:09d}: ", end="")

        return idx

    # Step 1: first 25 bytes
    i = print_bytes(i, 25)
    print("\n")

    while i < size:
        if i + 2 > size:
            break

        # Read N (but keep pointer)
        N = int.from_bytes(data[i:i+2], byteorder="little")

        # First 27 bytes (including those 2)
        i = print_bytes(i, 27)
        print("\n")

        # Remaining bytes
        remaining = max(0, N - 27)
        i = print_bytes(i, remaining, newline_every=12)
        print("\n")

    print()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <binary_file>")
        sys.exit(1)

    Format="""
#################################
##   FH_File header (25B)
##
##   EH_Event header (27B)
##
##   BD_Board-data(x1or2) (12B)
##   BD_Board-data(x1or2)
##   ...
##
##   EH_Event header (27B)
##   ....
#################################

    """

    print(Format)
    dump_file(sys.argv[1])
