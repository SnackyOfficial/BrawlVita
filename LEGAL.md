# Legal notes (not legal advice)

- The game, its libraries, assets, text and trademarks belong to Supercell. This repository and every release package contain **only our
  code** and plain placeholder artwork we drew. `libg.so`, `libfmod.so`, the game's assets and anything else from an APK must never be
  added to this repository, to a pull request or to a release.
- The installer reads **your own copies** of the APKs and writes files for **your own console**. It runs nothing from the APKs. It downloads
  the game's own high-resolution pictures from the game's asset server for you. Do not redistribute what it writes.
- The offline server is our own implementation, written from the game's observed behaviour. It does not contact any server.
- The loader changes the game library in memory on your console only (to run it offline and on the Vita); no changed copy is written or
  shared.
- Running a modified game client may breach the publisher's terms of service. This project works fully offline and has no online play.
- If you are a rights holder and have a concern, please open an issue.
