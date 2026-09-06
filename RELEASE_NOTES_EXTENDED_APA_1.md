# Open PS2 Loader Extended APA - Hardware-Tested Release 1

This is the first public hardware-tested release of **Open PS2 Loader Extended APA**.

It is intended for PlayStation 2 internal HDDs larger than 2 TiB that have been prepared using the matching **Extended APA / banked APA** layout.

## Real-hardware validation

Validated on a real PlayStation 2 with a 4 TB HDD:

- FHDB / FreeHDBoot boot works.
- Bank 0 HDL games are detected and launch.
- Bank 1 HDL games are detected and launch.
- OPL artwork and configuration remain in Bank 0 and work for Bank 1 games.
- Bank 0 remains the conventional APA/PFS system and application area.

## Layout

- **Bank 0:** conventional bootable APA/PFS + FHDB + OPL + applications/art/config + optional HDL games.
- **Bank 1+:** games-only APA banks.

Each full bank covers `0x100000000` 512-byte sectors (2 TiB). Upper-bank APA addresses remain bank-relative; OPL translates them to physical LBAs using the bank base.

```text
physical_lba = (bank_index * 0x100000000) + relative_lba
```

See `EXTENDED_APA.md` for the complete technical description and Mermaid diagram.

## Exact tested build

- OPL base version: `v1.2.0-Beta-2273`
- Extended APA source commit: `b2e39f0d6b569cf79a3235dc47050016d102cd31`
- Release ELF SHA-256: `aa7d6765f88fec8d059ebb4fafc76e75e5cd8f8820e083fec0f45640ed32a244`

The attached ELF is the exact CI-produced binary used for the successful hardware test. It is intentionally pinned so PS2 HDD Manager can download and verify a known-good Extended APA loader when provisioning drives larger than 2 TiB.

## Important

This remains experimental. It is not required for ordinary <=2 TiB APA HDDs. Use official Open PS2 Loader for normal configurations unless Extended APA support is specifically required.

A >2 TiB Extended APA disk must be prepared by a compatible formatter/writer. An official OPL build will not enumerate the upper APA banks.
