# Source and driver notices

Game-specific C sources, pixel vehicle silhouettes, ground renderer and tests
were authored for Nitro Nonsense. This integration imports the verified game
base from https://github.com/aberguecio/nitro-nonsense-plataboy at commit
`cb3ce64146609d894e2cb6101aafcf547aa1468a` (merged by PR #9).
No Godot source, original-game art, music or Hopper compatibility code is copied.
No new blanket license is assigned to the newly authored game-specific source.

The platform driver/startup files and linker are copied unchanged from this
repository's `games/template/` at `7ac6173372bd27256dee0bba279c595bdee27db1`,
retaining their original notices. The upstream README attributes the original
console design, Tiny games and drivers to Stefan Wagner / wagiminator,
https://github.com/wagiminator/CH32V003-GameConsole, under CC BY-SA 3.0:
https://creativecommons.org/licenses/by-sa/3.0/.
The game-local makefile selects the documented 48 MHz mode and supplies the game
sources without changing shared SDK code, startup or linker.

The simulator, library firmware and checker remain the repository's unchanged
reference components. This contribution does not change their licensing, alter
published ROMs or claim physical hardware validation.
