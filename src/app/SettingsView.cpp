// Settings: appearance, library sources, integrations and system options. Everything persists.
#include "App.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <functional>

#include "core/Util.h"

namespace lumen {

using namespace ui;

namespace {
struct Row {
    enum class Kind { Toggle, Choice, Accent, Action, Field, Info } kind = Kind::Info;
    std::wstring label, sub;
    bool on = false;
    std::function<void(bool)> onToggle;
    std::vector<std::wstring> options;
    int value = 0;
    std::function<void(int)> onChoice;
    std::wstring action;
    wchar_t glyph = 0;
    std::function<void()> onAction;
    std::wstring info;
};
struct Group {
    std::wstring title;
    std::vector<Row> rows;
};

Row Toggle_(std::wstring label, std::wstring sub, bool on, std::function<void(bool)> fn) {
    Row r;
    r.kind = Row::Kind::Toggle;
    r.label = std::move(label);
    r.sub = std::move(sub);
    r.on = on;
    r.onToggle = std::move(fn);
    return r;
}
Row Choice_(std::wstring label, std::wstring sub, std::vector<std::wstring> opts, int v, std::function<void(int)> fn) {
    Row r;
    r.kind = Row::Kind::Choice;
    r.label = std::move(label);
    r.sub = std::move(sub);
    r.options = std::move(opts);
    r.value = v;
    r.onChoice = std::move(fn);
    return r;
}
Row Action_(std::wstring label, std::wstring sub, std::wstring action, wchar_t glyph, std::function<void()> fn) {
    Row r;
    r.kind = Row::Kind::Action;
    r.label = std::move(label);
    r.sub = std::move(sub);
    r.action = std::move(action);
    r.glyph = glyph;
    r.onAction = std::move(fn);
    return r;
}
Row Info_(std::wstring label, std::wstring info) {
    Row r;
    r.kind = Row::Kind::Info;
    r.label = std::move(label);
    r.info = std::move(info);
    return r;
}
}  // namespace

void App::DrawSettings(Ctx& c) {
    Gfx& g = gfx_;
    InputFrame& in = c.in;
    auto dirty = [this] { settingsDirty_ = true; };

    // --- Model -----------------------------------------------------------------------------------------------
    std::vector<Group> groups;
    {
        Group a{L"APARÊNCIA"};
        a.rows.push_back(Choice_(L"Tema", L"O modo Sistema acompanha o Windows", {L"Escuro", L"Claro", L"Sistema"},
                                 static_cast<int>(settings_.theme), [this, dirty](int v) {
                                     settings_.theme = static_cast<ThemeMode>(v);
                                     ApplyTheme();
                                     dirty();
                                 }));
        Row accent;
        accent.kind = Row::Kind::Accent;
        accent.label = L"Cor de destaque";
        accent.sub = settings_.accent == kDynamicAccent ? L"Do jogo · muda com a arte do jogo selecionado"
                                                        : kAccentNames[settings_.accent];
        accent.value = settings_.accent;
        accent.onChoice = [this, dirty](int v) {
            settings_.accent = std::clamp(v, 0, kDynamicAccent);
            ApplyTheme();
            dirty();
        };
        a.rows.push_back(accent);
        a.rows.push_back(Choice_(L"Tamanho das capas", L"No catálogo e na biblioteca", {L"Compacto", L"Padrão", L"Grande"},
                                 settings_.cardSize, [this, dirty](int v) {
                                     settings_.cardSize = v;
                                     dirty();
                                 }));
        a.rows.push_back(Toggle_(L"Transparência e desfoque", L"Painéis de vidro sobre a arte do jogo", settings_.blur,
                                 [this, dirty](bool v) {
                                     settings_.blur = v;
                                     dirty();
                                 }));
        a.rows.push_back(Toggle_(L"Reduzir movimento", L"Desliga zoom, paralaxe e acelera transições",
                                 settings_.reduceMotion, [this, dirty](bool v) {
                                     settings_.reduceMotion = v;
                                     dirty();
                                 }));
        a.rows.push_back(Toggle_(L"Trailers no fundo",
                                 L"O trailer da loja toca atrás do jogo selecionado (sem som; baixado uma vez, ~45 s)",
                                 settings_.trailers, [this, dirty](bool v) {
                                     settings_.trailers = v;
                                     dirty();
                                 }));
        a.rows.push_back(Toggle_(L"Modo vitrine",
                                 L"Sem mexer por 45 s, o Início passa pelos seus jogos sozinho",
                                 settings_.showcaseMode, [this, dirty](bool v) {
                                     settings_.showcaseMode = v;
                                     dirty();
                                 }));
        groups.push_back(std::move(a));
    }
    {
        Group b{L"BIBLIOTECA"};
        auto count = [&](Filter f) {
            int n = library_.CountWhere(f);
            return std::to_wstring(n) + (n == 1 ? L" jogo" : L" jogos");
        };
        std::wstring steamSub = library_.steam().available() ? count(Filter::Steam) + L"  ·  " + library_.steam().steamPath()
                                                             : L"Steam não encontrada neste computador";
        b.rows.push_back(Toggle_(L"Steam", steamSub, settings_.sourceSteam, [this, dirty](bool v) {
            settings_.sourceSteam = v;
            dirty();
            RefreshAll(false);
        }));
        b.rows.push_back(Toggle_(L"Epic Games", count(Filter::Epic) + L"  ·  manifestos do Epic Games Launcher",
                                 settings_.sourceEpic, [this, dirty](bool v) {
                                     settings_.sourceEpic = v;
                                     dirty();
                                     RefreshAll(false);
                                 }));
        b.rows.push_back(Toggle_(L"GOG", count(Filter::Gog) + L"  ·  jogos registrados pelo GOG Galaxy", settings_.sourceGog,
                                 [this, dirty](bool v) {
                                     settings_.sourceGog = v;
                                     dirty();
                                     RefreshAll(false);
                                 }));
        b.rows.push_back(Toggle_(L"Buscar informações online",
                                 L"Descrições, gêneros e artes que faltam (Steam Store, CDN da Steam, GOG)",
                                 settings_.onlineData, [this, dirty](bool v) {
                                     settings_.onlineData = v;
                                     images_.SetOnline(v);
                                     trailers_.SetOnline(v);
                                     deals_.SetOnline(v);
                                     meta_.SetConfig(MetaConfig());
                                     dirty();
                                 }));
        b.rows.push_back(Toggle_(L"Mostrar aplicativos com os jogos",
                                 L"Programas da Steam (OBS, Blender…) ficam em Biblioteca › Aplicativos",
                                 settings_.showApps, [this, dirty](bool v) {
                                     settings_.showApps = v;
                                     dirty();
                                     RebuildHomeList();
                                 }));
        b.rows.push_back(Action_(L"Atualizar biblioteca", L"Procura jogos novos ou removidos (F5)", L"Atualizar",
                                 glyph::Refresh, [this] { RefreshAll(true); }));
        b.rows.push_back(Action_(L"Adicionar jogo manualmente", L"Qualquer executável: emuladores, jogos sem loja… (Ctrl+N)",
                                 L"Escolher…", glyph::Add, [this] { AddGameDialog(); }));
        groups.push_back(std::move(b));
    }
    {
        Group i{L"INTEGRAÇÕES"};
        Row key;
        key.kind = Row::Kind::Field;
        key.label = L"Chave da Steam Web API";
        key.sub = settings_.steamApiKey.empty()
                      ? L"Opcional: mostra a lista completa de conquistas. Guardada com criptografia do Windows."
                      : L"Configurada  ·  Lista completa de conquistas ativada";
        i.rows.push_back(key);
        i.rows.push_back(Action_(L"Obter uma chave", L"steamcommunity.com/dev/apikey  ·  o perfil precisa ter “Detalhes do jogo” público",
                                 L"Abrir site", glyph::Link, [] {
                                     ShellExecuteW(nullptr, L"open", L"https://steamcommunity.com/dev/apikey", nullptr,
                                                   nullptr, SW_SHOWNORMAL);
                                 }));
        if (!settings_.steamApiKey.empty())
            i.rows.push_back(Action_(L"Remover chave", L"Volta a usar apenas o cache local da Steam", L"Remover",
                                     glyph::Delete, [this, dirty] {
                                         settings_.steamApiKey.clear();
                                         meta_.SetConfig(MetaConfig());
                                         meta_.ForgetAll();
                                         dirty();
                                         ShowToast(L"Chave removida", glyph::Key);
                                     }));
        groups.push_back(std::move(i));
    }
    {
        Group w{L"BEM-ESTAR"};
        w.rows.push_back(Choice_(L"Lembrete de tempo de jogo", L"Notificação do Windows enquanto você joga",
                                 {L"Desligado", L"30 min", L"1 h", L"2 h"}, settings_.remindInterval,
                                 [this, dirty](int v) {
                                     settings_.remindInterval = v;
                                     dirty();
                                 }));
        w.rows.push_back(Choice_(L"Meta diária", L"Aviso e anel de progresso quando passar do limite do dia",
                                 {L"Sem meta", L"1 h", L"2 h", L"3 h", L"4 h"}, settings_.dailyGoal,
                                 [this, dirty](int v) {
                                     settings_.dailyGoal = v;
                                     dirty();
                                 }));
        w.rows.push_back(Toggle_(L"Resumo ao fechar o jogo", L"Avisa quanto tempo durou a sessão (a partir de 5 min)",
                                 settings_.sessionSummary, [this, dirty](bool v) {
                                     settings_.sessionSummary = v;
                                     dirty();
                                 }));
        w.rows.push_back(Toggle_(L"Continuar na bandeja ao fechar",
                                 L"Mantém os avisos funcionando com a janela fechada (Sair pelo ícone da bandeja)",
                                 settings_.keepInTray, [this, dirty](bool v) {
                                     settings_.keepInTray = v;
                                     dirty();
                                 }));
        w.rows.push_back(Action_(L"Testar notificação", L"Mostra um exemplo de aviso agora", L"Testar", glyph::Bell, [this] {
            SendTestNotification();
            ShowToast(L"Notificação enviada", glyph::Bell);
        }));
        groups.push_back(std::move(w));

        Group s{L"SISTEMA"};
        s.rows.push_back(Action_(L"Tela cheia", L"Também pelo botão no topo, F11 ou Alt+Enter",
                                 fullscreen_ ? L"Sair da tela cheia" : L"Entrar", fullscreen_ ? glyph::BackToWindow : glyph::FullScreen,
                                 [this] { ToggleFullscreen(); }));
        s.rows.push_back(Toggle_(L"Abrir em tela cheia", L"Ideal para TV e controle",
                                 settings_.startFullscreen, [this, dirty](bool v) {
                                     settings_.startFullscreen = v;
                                     dirty();
                                 }));
        s.rows.push_back(Toggle_(L"Tela cheia ao conectar um controle", L"Vira um console na hora em que o controle liga",
                                 settings_.fullscreenOnController, [this, dirty](bool v) {
                                     settings_.fullscreenOnController = v;
                                     dirty();
                                 }));
        s.rows.push_back(Toggle_(L"Iniciar com o Windows", L"Abre direto na bandeja, para os avisos funcionarem sempre",
                                 LaunchAtStartup(), [this](bool v) {
                                     SetLaunchAtStartup(v);
                                     ShowToast(v ? L"O Lumen vai iniciar com o Windows" : L"Não vai mais iniciar com o Windows",
                                               glyph::Checkmark);
                                 }));
        s.rows.push_back(Toggle_(L"Sons da interface", L"Cliques suaves ao navegar, abrir e jogar", settings_.uiSounds,
                                 [this, dirty](bool v) {
                                     settings_.uiSounds = v;
                                     dirty();
                                     if (v) PlaySfx(Sfx::Select);
                                 }));
        s.rows.push_back(Action_(L"Pasta de dados", AppDataDir(), L"Abrir", glyph::Folder, [] {
            ShellExecuteW(nullptr, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }));
        groups.push_back(std::move(s));
    }
    {
        Group k{L"ATALHOS"};
        k.rows.push_back(Info_(L"Navegar", L"Setas  ·  D-pad  ·  analógico"));
        k.rows.push_back(Info_(L"Abrir jogo  ·  Jogar", L"Enter  ·  A"));
        k.rows.push_back(Info_(L"Voltar", L"Esc  ·  B"));
        k.rows.push_back(Info_(L"Página do jogo", L"I  ·  Y"));
        k.rows.push_back(Info_(L"Favoritar", L"F  ·  X"));
        k.rows.push_back(Info_(L"Opções do jogo", L"M  ·  Start  ·  clique direito"));
        k.rows.push_back(Info_(L"Buscar", L"Ctrl+F  ·  /  ·  digite na Biblioteca"));
        k.rows.push_back(Info_(L"Trocar de seção", L"Tab  ·  Ctrl+1…5  ·  View"));
        k.rows.push_back(Info_(L"Surpreenda-me", L"R"));
        k.rows.push_back(Info_(L"Tela cheia", L"F11  ·  Alt+Enter"));
        groups.push_back(std::move(k));
    }
    {
        Group ab{L"SOBRE"};
        ab.rows.push_back(Info_(L"Lumen 1.0", L"C++17  ·  Direct2D  ·  DirectWrite"));
        groups.push_back(std::move(ab));
    }

    std::vector<Row*> focusables;
    for (auto& gr : groups)
        for (auto& r : gr.rows)
            if (r.kind != Row::Kind::Info) focusables.push_back(&r);
    int nf = static_cast<int>(focusables.size());
    set_.focus = std::clamp(set_.focus, 0, std::max(0, nf - 1));

    // --- Input -------------------------------------------------------------------------------------------
    auto saveKey = [&] {
        settings_.steamApiKey = Narrow(Trim(set_.keyDraft));
        set_.editingKey = false;
        meta_.SetConfig(MetaConfig());
        meta_.ForgetAll();
        dirty();
        ShowToast(settings_.steamApiKey.empty() ? L"Chave removida" : L"Chave salva com segurança", glyph::Key);
    };
    if (!sheet_.open) {
        if (set_.editingKey) {
            for (wchar_t ch : in.text)
                if (set_.keyDraft.size() < 64 && iswalnum(ch)) set_.keyDraft += ch;
            if (in.Has(Key::Backspace) && !set_.keyDraft.empty()) set_.keyDraft.pop_back();
            if (in.Has(Key::Paste) && OpenClipboard(hwnd_)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                        set_.keyDraft = Trim(p).substr(0, 64);
                        GlobalUnlock(h);
                    }
                }
                CloseClipboard();
            }
            if (in.Has(Key::Accept)) saveKey();
            if (in.Has(Key::Escape)) set_.editingKey = false;
        } else if (nf > 0) {
            if (in.Has(Key::Up)) set_.focus = std::max(0, set_.focus - 1);
            if (in.Has(Key::Down)) set_.focus = std::min(nf - 1, set_.focus + 1);
            Row& r = *focusables[set_.focus];
            int dir = in.Has(Key::Left) ? -1 : in.Has(Key::Right) ? 1 : 0;
            if (dir && r.kind == Row::Kind::Choice)
                r.onChoice(std::clamp(r.value + dir, 0, static_cast<int>(r.options.size()) - 1));
            if (dir && r.kind == Row::Kind::Accent) r.onChoice((r.value + dir + kAccentCount + 1) % (kAccentCount + 1));
            if (in.Has(Key::Accept)) {
                switch (r.kind) {
                    case Row::Kind::Toggle: r.onToggle(!r.on); break;
                    case Row::Kind::Choice: r.onChoice((r.value + 1) % static_cast<int>(r.options.size())); break;
                    case Row::Kind::Accent: r.onChoice((r.value + 1) % (kAccentCount + 1)); break;
                    case Row::Kind::Action: r.onAction(); break;
                    case Row::Kind::Field:
                        set_.editingKey = true;
                        set_.keyDraft = Widen(settings_.steamApiKey);
                        break;
                    default: break;
                }
            }
            if (in.Has(Key::Back)) SwitchTab(Tab::Home);
        }
        set_.scrollTarget -= in.wheel * 150.f;
    }

    // --- Draw ---------------------------------------------------------------------------------------------
    float colW = std::min(960.f, g.W - 2 * 72.f);
    float x0 = (g.W - colW) * 0.5f;
    float rowH = 74;
    float total = 200;
    for (auto& gr : groups) total += 46 + gr.rows.size() * rowH + 28;
    set_.scrollTarget = std::clamp(set_.scrollTarget, 0.f, std::max(0.f, total - g.H + 60));
    set_.scroll += (set_.scrollTarget - set_.scroll) * (1.f - std::exp(-c.dt * 14.f));
    g.PushTransform(D2D1::Matrix3x2F::Translation(0, -set_.scroll));
    c.scrollY = set_.scroll;  // hit testing follows the scrolled content

    TextStyle title = Body(46, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    title.font = FontRole::Display;
    g.Text(L"Ajustes", {x0, 112, colW, 60}, title);

    bool kb = !in.mouseMode;
    float y = 200;
    int fi = 0;
    float focusTop = -1, focusBottom = -1;
    for (auto& gr : groups) {
        g.Text(gr.title, {x0 + 20, y + 14, colW, 16}, Caps(12, theme_.text2));
        y += 42;
        Rect panel{x0, y, colW, gr.rows.size() * rowH};
        g.FillRound(panel, 22, theme_.dark ? Color::White(0.06f) : Color::White(0.9f));
        g.StrokeRound(panel, 22, theme_.glassStroke, 1);
        for (size_t ri = 0; ri < gr.rows.size(); ++ri) {
            Row& r = gr.rows[ri];
            Rect row{x0, y, colW, rowH};
            if (ri > 0) g.FillRect({x0 + 24, y, colW - 48, 1}, theme_.separator);
            bool interactive = r.kind != Row::Kind::Info;
            int myIndex = interactive ? fi++ : -1;
            bool focused = interactive && kb && myIndex == set_.focus && !sheet_.open;
            if (focused) {
                focusTop = row.y;
                focusBottom = row.b();
            }
            float f = c.Anim(Id("setrow", static_cast<uint64_t>(myIndex + 1000 * (interactive ? 1 : 0)) ^ Hash64(Narrow(r.label))),
                             focused ? 1.f : 0.f, 18.f);
            if (f > 0.01f) g.FillRound(row.Inset(6, 4), 16, theme_.control.Fade(f));
            if (interactive && c.Hover(row) && in.mouseMode) set_.focus = myIndex;

            float textW = colW * 0.5f;
            TextStyle ls = Body(17, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            if (r.sub.empty()) {
                ls.valign = VAlign::Center;
                g.Text(r.label, {x0 + 26, y, textW, rowH}, ls);
            } else {
                g.Text(r.label, {x0 + 26, y + 14, textW, 24}, ls);
                g.Text(r.sub, {x0 + 26, y + 40, colW * (r.kind == Row::Kind::Info ? 0.5f : 0.55f), 20}, Body(13, theme_.text2));
            }
            float right = x0 + colW - 24;
            switch (r.kind) {
                case Row::Kind::Toggle:
                    if (Toggle(c, Id("settoggle", myIndex), {right - 60, y, 60, rowH}, r.on, false) ||
                        (c.Click(row) && !Rect{right - 60, y, 60, rowH}.Contains(c.Mouse())))
                        r.onToggle(!r.on);
                    if (c.Hover(row)) c.wantHand = true;
                    break;
                case Row::Kind::Choice: {
                    float w = std::min(112.f * r.options.size(), 500.f);
                    int clicked = Segmented(c, Id("setseg", myIndex), {right - w, y + 15, w, 44}, r.options, r.value, false);
                    if (clicked >= 0 && clicked != r.value) r.onChoice(clicked);
                    break;
                }
                case Row::Kind::Accent: {
                    float d = 30, gap = 14, w = kAccentCount * d + (kAccentCount - 1) * gap;
                    for (int i = 0; i < kAccentCount; ++i) {
                        Vec2 cc{right - w + d * 0.5f + i * (d + gap), y + rowH * 0.5f};
                        Rect hit{cc.x - d * 0.5f - 4, cc.y - d * 0.5f - 4, d + 8, d + 8};
                        float s = c.Anim(Id("accent", i), i == r.value ? 1.f : 0.f, 18.f);
                        if (i < kAccentCount) {
                            g.FillCircle(cc, d * 0.5f, kAccents[i]);
                        } else {
                            Rect sw{cc.x - d * 0.5f, cc.y - d * 0.5f, d, d};
                            g.Gradient(sw, d * 0.5f, {sw.x, sw.y}, {sw.r(), sw.b()},
                                       {{0.f, Color::Hex(0xFF9F0A)}, {0.5f, Color::Hex(0xBF5AF2)}, {1.f, Color::Hex(0x0A84FF)}});
                        }
                        if (s > 0.01f) g.StrokeCircle(cc, d * 0.5f + 5, theme_.text.Fade(s), 2.f);
                        if (c.Hover(hit)) c.wantHand = true;
                        if (c.Click(hit)) r.onChoice(i);
                    }
                    break;
                }
                case Row::Kind::Action: {
                    BtnSpec b{r.action, r.glyph, BtnKind::Glass};
                    b.fontSize = 15;
                    float w = g.Measure(r.action, Body(15, theme_.text, DWRITE_FONT_WEIGHT_SEMI_BOLD)).x + (r.glyph ? 72 : 48);
                    if (Button(c, Id("setact", myIndex), {right - w, y + 15, w, 44}, b)) r.onAction();
                    break;
                }
                case Row::Kind::Field: {
                    Rect fr{right - 400, y + 13, 400, 48};
                    std::wstring shown = set_.editingKey ? set_.keyDraft : Widen(settings_.steamApiKey);
                    if (TextField(c, Id("apikey"), fr, shown, L"Cole sua chave aqui", set_.editingKey, glyph::Key,
                                  !set_.editingKey)) {
                        if (!set_.editingKey) {
                            set_.editingKey = true;
                            set_.keyDraft = Widen(settings_.steamApiKey);
                        }
                    }
                    if (set_.editingKey) {
                        TextStyle hint = Body(12, theme_.text3);
                        hint.align = Align::Trailing;
                        g.Text(L"Enter salva  ·  Esc cancela  ·  Ctrl+V cola", {fr.x, fr.b() + 2, fr.w, 16}, hint);
                    }
                    if (set_.editingKey && in.released && !fr.Contains(c.Mouse()) && !c.blockMouse) saveKey();
                    break;
                }
                case Row::Kind::Info: {
                    TextStyle is = Body(15, theme_.text2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                    is.align = Align::Trailing;
                    is.valign = VAlign::Center;
                    g.Text(r.info, {x0 + colW * 0.42f, y, colW * 0.58f - 26, rowH}, is);
                    break;
                }
            }
            y += rowH;
        }
        y += 28;
    }
    g.PopTransform();
    c.scrollY = 0;
    // Keep keyboard focus visible.
    if (kb && focusTop >= 0) {
        if (focusTop - set_.scrollTarget < 110) set_.scrollTarget = focusTop - 110;
        if (focusBottom - set_.scrollTarget > g.H - 40) set_.scrollTarget = focusBottom - g.H + 40;
    }
}

}  // namespace lumen
