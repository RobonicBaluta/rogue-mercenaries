# Third-party code

Vendored, unmodified upstream release files. Do not edit them; update by downloading a new
release and recording it here. Project warning flags (`/W4 /WX`) are not applied to these files
(they are included as SYSTEM headers or compiled in separate targets).

| Component | Version | Files | Source URL | License | Used by |
|---|---|---|---|---|---|
| nlohmann/json | v3.11.3 | `nlohmann/include/nlohmann/json.hpp` (SHA-256 `9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`) | https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp | MIT (`nlohmann/LICENSE.MIT`, from https://raw.githubusercontent.com/nlohmann/json/v3.11.3/LICENSE.MIT) | `rm_core` (private), `rm_tests`, `rm_sim` |
| MinHook | v1.3.4 | `minhook/include`, `minhook/src` (release archive SHA-256 `172708123daa0c98d20d3a980b16a50be14af243dc95dee6f79c24193ad010e4`) | https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip | BSD-2-Clause (`minhook/LICENSE.txt`) | `rm_re5` (shipped inside `dinput8.dll`) |
| Dear ImGui | v1.91.9 | core sources plus `backends/imgui_impl_dx9.*`, `backends/imgui_impl_win32.*` (release archive SHA-256 `3fe9e93066e37c630cbbada821a45812cd86df0f21a476cf74c925e1b649878d`) | https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.zip | MIT (`imgui/LICENSE.txt`) | `rm_re5` (shipped inside `dinput8.dll`) |
| Catch2 (amalgamated) | v3.7.1 | `catch2/catch_amalgamated.hpp` (SHA-256 `91d47e2ddfb2949df22ff582d6c292554184da743e11d3811dcdd06b5b0a159b`), `catch2/catch_amalgamated.cpp` (SHA-256 `450fc5f213a87668f0f48c91a2afa4f4226186fef453d36e6fa3bc184ccdb9ba`) | https://github.com/catchorg/Catch2/releases/download/v3.7.1/catch_amalgamated.hpp and `.../catch_amalgamated.cpp` | BSL-1.0 (`catch2/LICENSE.txt`, from https://raw.githubusercontent.com/catchorg/Catch2/v3.7.1/LICENSE.txt) | `rm_tests` only (not shipped) |

Downloaded 2026-10-27 with `curl -sSfL` from the URLs above; hashes from `sha256sum`.
