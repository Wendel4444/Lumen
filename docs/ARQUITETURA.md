# Lumen — Arquitetura

Lumen é um launcher/biblioteca visual de jogos para Windows, escrito em C++17,
com interface própria inspirada na linguagem visual da Apple (tvOS / macOS).

---

## 1. Análise e desafios técnicos

| Desafio | Por que é difícil | Como resolvemos |
|---|---|---|
| Visual premium e fluido | Componentes prontos (Win32 comum, widgets) parecem "painel administrativo" | Renderização própria via GPU (Direct2D 1.1 sobre Direct3D 11, swap chain flip-model, vsync) |
| Desfoque/transparência | Blur em tempo real é caro | Fundo desfocado é pré-calculado **uma vez por jogo** em textura reduzida (Gaussian blur do D2D) e reaproveitado em todos os painéis "vidro" |
| Capas em alta qualidade sem travar | Heróis da Steam têm 3840×1240 (19 MB de VRAM cada) | Decodificação assíncrona (WIC, threads de trabalho), upload incremental para a GPU, cache LRU que libera o que não está visível |
| Detectar jogos | Cada loja guarda dados de um jeito | Provedores independentes (Steam/Epic/GOG/Manual) que produzem o mesmo modelo `Game` |
| Conquistas reais | Não há API local oficial; a Web API exige chave | 1) Cache local que o próprio cliente Steam mantém (contagem exata + lista parcial) 2) Steam Web API opcional (lista completa) — nunca dados inventados |
| Tempo de jogo | Só a Steam registra | Steam: `localconfig.vdf`. Jogos manuais/GOG: o Lumen mede a sessão (handle do processo). Steam em execução: `RunningAppID` no registro |
| Persistência | Favoritos, categorias, ajustes, histórico | JSON em `%LOCALAPPDATA%\Lumen`, gravação atômica (arquivo temporário + `MoveFileEx`), chave de API protegida com DPAPI |
| Distribuição | Dependências pesadas complicam instalação | Um único `.exe` (~1 MB), sem DLLs de terceiros — só APIs do Windows 10/11 |

## 2. Escolha da tecnologia de interface

| Opção | Prós | Contras | Veredito |
|---|---|---|---|
| **Qt 6 + QML** | Declarativo, animações e `MultiEffect` (blur) prontos, ótima produtividade | SDK de vários GB (instalador exige conta Qt), distribuição de 60–120 MB com `windeployqt`, motor QML consome ~100 MB+ de RAM, licença LGPL exige cuidado com linkagem | Excelente segunda opção |
| WinUI 3 (C++/WinRT) | Acrylic/Mica nativos, visual Windows 11 | Exige VS 2022 + Windows App SDK + runtime extra; visual "Windows", não identidade própria; C++/WinRT verboso | Não |
| Dear ImGui | Leve, rápido de iterar | Imediato e "ferramental"; tipografia/animações/blur premium exigiriam reescrever quase tudo | Não |
| HTML (CEF/WebView2/Ultralight) | CSS facilita o visual | Chromium = 150+ MB de RAM; a lógica vira JS, não C++ | Não |
| **Direct2D + DirectWrite + WIC (escolhido)** | Zero dependências externas, GPU, tipografia DirectWrite de altíssima qualidade (Segoe UI Variable), efeitos (blur) nativos, executável pequeno, consumo baixíssimo, controle total do visual | Precisamos escrever nossa camada de UI (animações, foco, widgets) | **Escolhido** |

**Justificativa:** o requisito central é identidade visual própria, fluidez e baixo consumo.
Com Direct2D, cada pixel é nosso (nada de componentes prontos), roda a 60+ FPS com uso mínimo de CPU,
e o build funciona com as ferramentas que já existem nesta máquina (MSVC 2019 + Windows SDK).
O núcleo (`src/core`) não depende da interface. Uma futura UI em Qt/QML poderia reutilizá-lo sem mudanças.

## 3. Arquitetura

```
┌──────────────────────────── app (orquestração) ───────────────────────────┐
│ App: janela, loop, rotas (Início/Biblioteca/Estatísticas/Ajustes/Página),  │
│ folhas modais (sheets), toasts, salvamento com debounce                    │
├──────────────── ui (apresentação) ─────────────┬──── core (domínio) ───────┤
│ Gfx: D3D11/D2D, texto, primitivas, blur        │ Library: agrega, filtra,  │
│ ImageCache: decodificação/download assíncronos │   ordena, pesquisa        │
│ Input: teclado, mouse, XInput (controle)       │ Providers: Steam, Epic,   │
│ Ui: animações, widgets (botões, vidro, chips)  │   GOG, Manual             │
│ Screens: Showcase, Library, Stats, Settings    │ UserStore / Settings (JSON│
│ Theme: tokens claro/escuro + cor de destaque   │   persistente, DPAPI)     │
│                                                │ Metadata: detalhes e      │
│                                                │   conquistas (thread)     │
│                                                │ Launcher: inicia, mede    │
│                                                │   sessões, detecta Steam  │
│                                                │ Json / Vdf / Http / Util  │
└────────────────────────────────────────────────┴───────────────────────────┘
```

Regras:
- `core` não inclui nada de `ui`. A UI lê o estado do `core` e chama métodos de alto nível
  (`ToggleFavorite`, `Launch`, `AddCustomGame`...).
- Trabalho de E/S (rede, decodificação de imagem, leitura de cache) fica em threads de trabalho.
  Os resultados voltam ao thread da UI por filas protegidas por mutex. A UI nunca bloqueia.
- Cada provedor implementa `Scan(std::vector<Game>&)`. Nova plataforma = novo arquivo.

## 4. Estrutura de pastas

```
Espelhamento games/
├─ build.bat              build com MSVC (sem CMake)
├─ CMakeLists.txt         build alternativo (VS/CMake)
├─ README.md
├─ docs/ARQUITETURA.md
├─ res/                   manifesto (DPI, Windows 10/11), ícone, .rc
└─ src/
   ├─ main.cpp
   ├─ core/               domínio, sem UI
   │  ├─ Json.*  Vdf.*  Util.*  Http.*
   │  ├─ Game.h  Settings.*  UserStore.*  Library.*
   │  ├─ Launcher.*  Metadata.*
   │  └─ providers/  Steam.*  Epic.* (Epic + GOG)
   ├─ ui/                 motor de interface
   │  ├─ Gfx.*  ImageCache.*  Input.*  Ui.* (tema, animações, widgets)
   └─ app/                telas e orquestração
      ├─ App.* (janela, rotas, menus, toasts)  Showcase.cpp  LibraryView.cpp  StatsView.cpp  SettingsView.cpp
```

## 5. Dados e integrações

### Detecção
- **Steam:** `HKCU\Software\Valve\Steam\SteamPath` → `steamapps/libraryfolders.vdf` → `appmanifest_*.acf`
  em cada biblioteca (nome, pasta, tamanho). Ferramentas (Proton, redistribuíveis) são filtradas, e o
  `appcache/appinfo.vdf` (binário, v27–v29) diz o tipo de cada app, para separar programas (OBS, Blender) dos jogos.
- **Epic:** `%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item` (JSON). DLCs são ignoradas.
- **GOG:** `HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\*`.
- **Manual:** qualquer `.exe` escolhido pelo usuário (diálogo nativo), salvo em `library.json`.

### Capas e artes
1. Arte escolhida pelo usuário (copiada para `%LOCALAPPDATA%\Lumen\art\<id>\`).
2. Steam: cache local `appcache/librarycache/<appid>/` (`library_600x900`, `library_hero`, `logo`, `header`).
   Se faltar, CDN pública da Steam (baixada uma vez e guardada em disco).
3. Epic: `Data/Catalog/catcache.bin` (JSON em base64 com as `keyImages` oficiais).
4. GOG: `api.gog.com/products/<id>` (público).
5. Sem arte: composição tipográfica gerada (gradiente + nome + ícone do executável).

### Conquistas (somente dados reais)
- **Cache local da Steam** (`userdata/<id>/config/librarycache/<appid>.json`): contagem exata
  `nAchieved/nTotal` e uma lista parcial com nome, descrição, ícone, data e % global.
- **Steam Web API** (opcional, chave do usuário em Ajustes, protegida com DPAPI):
  `GetSchemaForGame` + `GetPlayerAchievements` + `GetGlobalAchievementPercentagesForApp` → lista completa.
  Exige que os "Detalhes do jogo" do perfil estejam públicos.
- Epic/GOG/manual: não há API pública de conquistas sem login OAuth. A interface mostra isso claramente.

### Tempo de jogo
- Steam: `userdata/<id>/config/localconfig.vdf` (`Playtime`, `LastPlayed`).
- Jogos iniciados pelo Lumen com executável próprio: sessão medida pelo handle do processo.
- Steam em execução: `HKCU\Software\Valve\Steam\RunningAppID` (mostra "Jogando" e atualiza ao fechar).

### Inicialização
- Steam: `steam://rungameid/<appid>` · Epic: `com.epicgames.launcher://apps/<ns>:<item>:<app>?action=launch`
- GOG e manuais: `ShellExecuteEx` no executável com a pasta do jogo como diretório de trabalho.

### Persistência (`%LOCALAPPDATA%\Lumen`)
- `settings.json`: tema, cor de destaque, tamanho das capas, ordenação, fontes, chave de API (DPAPI).
- `library.json`: favoritos, categorias, ocultos, data de adição, contagem de execuções, sessões,
  jogos manuais, histórico de execuções.
- `cache/`: detalhes da loja, conquistas da Web API, imagens baixadas.

## 6. Plano de desenvolvimento

1. **Base (esta entrega):** motor gráfico, janela, input (teclado, mouse, controle), cache de imagens,
   detecção Steam/Epic/GOG/manual, persistência, catálogo horizontal, página do jogo, botão Jogar.
2. **Integrações:** conquistas (cache local + Web API), detalhes da loja, artes da CDN, coleções da Steam.
3. **Organização:** biblioteca em grade/lista, pesquisa, filtros, categorias, favoritos, ordenação.
4. **Extras:** estatísticas, histórico, ajustes, temas, cor de destaque, "Surpreenda-me", medição de sessões.
5. **Próximos passos:** instalador (MSIX/Inno Setup), SteamGridDB opcional, Xbox/EA/Ubisoft, cache de miniaturas.

## 7. Direção visual

- **Palco:** a arte do jogo ocupa a tela inteira. A interface fica por cima, em camadas de vidro discretas.
- **Tipografia:** Segoe UI Variable (Display para títulos, Text para corpo). Rótulos em versalete
  com espaçamento largo (11–13 px, +1.6 de tracking). Números grandes e semibold.
- **Cor:** neutros quase pretos (#0B0B0D) e brancos com opacidades (100/62/38 %). Uma única cor de destaque
  (padrão: vermelho #E8412C), usada só na ação principal, no progresso e em rótulos de contexto.
- **Forma:** cantos de 14 px nos cards, 22 px nos painéis, botões em pílula, ícones Segoe Fluent.
- **Profundidade:** sombras suaves, anel de foco branco, leve escala (1.0 → 1.04) no item focado.
- **Movimento:** interpolação exponencial (sem "molas" exageradas), crossfade de fundos, Ken Burns sutil.
  "Reduzir movimento" desliga zoom/parallax.
- **Foco:** navegação por teclado/controle com foco visível; mouse com hover discreto.
