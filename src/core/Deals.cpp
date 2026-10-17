#include "Deals.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "Http.h"
#include "Json.h"
#include "Util.h"

namespace lumen {

namespace {
constexpr int64_t kCacheSeconds = 3 * 3600;

std::wstring CachePath(const std::string& id) { return JoinPath(AppSubDir(L"cache"), Widen("deals_" + id + ".json")); }

// Only entities, keeps the text (unlike StripHtml).
std::string DecodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t e = s.find(';', i);
            if (e != std::string::npos && e - i <= 8) {
                std::string ent = s.substr(i + 1, e - i - 1);
                std::string rep;
                if (ent == "quot") rep = "\"";
                else if (ent == "amp") rep = "&";
                else if (ent == "lt") rep = "<";
                else if (ent == "gt") rep = ">";
                else if (ent == "apos" || ent == "#39" || ent == "#039") rep = "'";
                else if (ent == "nbsp") rep = " ";
                else if (!ent.empty() && ent[0] == '#') {
                    unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16)
                                                                                       : strtoul(ent.c_str() + 1, nullptr, 10);
                    if (cp > 0 && cp < 0x10000) rep = Narrow(std::wstring(1, static_cast<wchar_t>(cp)));
                }
                if (!rep.empty()) {
                    out += rep;
                    i = e + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return out;
}

// First balanced {...} of a string (respecting JSON strings).
std::string FirstObject(const std::string& s) {
    size_t start = s.find('{');
    if (start == std::string::npos) return {};
    int depth = 0;
    bool inStr = false;
    for (size_t i = start; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            if (c == '\\') ++i;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return s.substr(start, i - start + 1);
    }
    return {};
}

// "R$ 1.234,56" / "€12.34" -> number (handles both decimal conventions).
double ParsePrice(const std::string& text) {
    std::string digits;
    for (char c : text)
        if ((c >= '0' && c <= '9') || c == ',' || c == '.') digits += c;
    if (digits.empty()) return 0;
    size_t lastSep = digits.find_last_of(",.");
    std::string intPart = lastSep == std::string::npos ? digits : digits.substr(0, lastSep);
    std::string frac = lastSep == std::string::npos ? "" : digits.substr(lastSep + 1);
    if (frac.size() == 3) {  // "1.234" -> thousands separator, not decimals
        intPart += frac;
        frac.clear();
    }
    intPart.erase(std::remove_if(intPart.begin(), intPart.end(), [](char c) { return c == ',' || c == '.'; }), intPart.end());
    return std::atof((intPart + (frac.empty() ? "" : "." + frac)).c_str());
}

std::wstring Symbol(const std::string& code) {
    if (code == "BRL") return L"R$";
    if (code == "EUR") return L"€";
    if (code == "USD") return L"US$";
    if (code == "GBP") return L"£";
    return Widen(code);
}

std::string Between(const std::string& s, size_t from, const std::string& a, const std::string& b, size_t limit) {
    size_t i = s.find(a, from);
    if (i == std::string::npos || i > limit) return {};
    i += a.size();
    size_t e = s.find(b, i);
    if (e == std::string::npos || e > limit) return {};
    return s.substr(i, e - i);
}

// Gift cards, wallet top-ups and subscriptions are not games.
bool IsNonGame(const std::wstring& title) {
    for (const wchar_t* w : {L"Gift Card", L"Cartão", L"Cartao", L"eShop", L"Wallet", L"Points",
                             L"Pontos", L"Subscription", L"Assinatura", L"Game Pass", L"Nitro", L"PSN ", L"Xbox Live",
                             L"V-Bucks", L"Robux", L"Top Up", L"Recarga"})
        if (ContainsInsensitive(title, w)) return true;
    return false;
}

std::wstring CleanTitle(std::wstring t) {
    for (const wchar_t* junk : {L" - Digital Key", L" Digital Key", L" (PC / Mac / Linux)", L" (PC / Mac)", L" (PC/Mac)",
                                L" (PC)"}) {
        size_t p;
        while ((p = t.find(junk)) != std::wstring::npos) t.erase(p, wcslen(junk));
    }
    return Trim(t);
}

// --- Store parsers ----------------------------------------------------------------------------------

// Green Man Gaming: every product block carries its data in ng-init="initialize({...})".
std::vector<Deal> ParseGmg(const std::string& html) {
    std::vector<Deal> out;
    std::set<std::wstring> seen;
    std::string currency = Between(html, 0, "currency=\"'", "'\"", html.size());
    const std::string needle = "initialize({&quot;Url";
    for (size_t i = html.find(needle); i != std::string::npos; i = html.find(needle, i + 1)) {
        size_t end = html.find('"', i);
        if (end == std::string::npos) break;
        std::string obj = FirstObject(DecodeEntities(html.substr(i + 11, end - i - 11)));
        json::Value v;
        if (obj.empty() || !json::Parse(obj, v)) continue;
        Deal d;
        d.title = Widen(v["GameName"].str());
        d.url = L"https://www.greenmangaming.com/pt" + Widen(v["Url"].str());
        d.image = Widen(v["BigModuleHalfImage"].str(v["CarouselDesktopImage"].str()));
        d.price = v["Price"].asNumber();
        d.oldPrice = v["OldPrice"].asNumber();
        d.discount = static_cast<int>(v["DiscountPercentOff"].asInt());
        d.currency = Symbol(currency.empty() ? "BRL" : currency);
        d.platform = L"PC";
        if (d.title.empty() || d.price <= 0 || d.discount <= 0 || IsNonGame(d.title) || !seen.insert(d.url).second) continue;
        out.push_back(std::move(d));
    }
    return out;
}

// Instant Gaming: server-rendered <article class="item"> cards (prices in the visitor's currency).
std::vector<Deal> ParseInstantGaming(const std::string& html) {
    std::vector<Deal> out;
    std::set<std::wstring> seen;
    const std::string open = "<article class=\"item\">";
    for (size_t i = html.find(open); i != std::string::npos; i = html.find(open, i + 1)) {
        size_t end = html.find("</article>", i);
        if (end == std::string::npos) break;
        Deal d;
        d.url = Widen(Between(html, i, "href=\"", "\"", end));
        d.title = Widen(DecodeEntities(Between(html, i, "class=\"title\" title=\"", "\"", end)));
        d.image = Widen(Between(html, i, "data-src=\"", "\"", end));
        // The price div's class attribute is padded with spaces/newlines: compare the trimmed value.
        std::string priceText;
        for (size_t p = html.find("class=\"", i); p != std::string::npos && p < end; p = html.find("class=\"", p + 7)) {
            size_t q = html.find('"', p + 7);
            if (q == std::string::npos || q > end) break;
            std::string cls = html.substr(p + 7, q - p - 7);
            cls.erase(0, cls.find_first_not_of(" \t\r\n"));
            cls.erase(cls.find_last_not_of(" \t\r\n") + 1);
            if (cls == "price") {
                priceText = DecodeEntities(Between(html, q, ">", "<", end));
                break;
            }
        }
        std::string disc = Between(html, i, "class=\"discount\">", "<", end);
        d.price = ParsePrice(priceText);
        d.discount = std::abs(std::atoi(Narrow(Trim(Widen(disc))).c_str()));  // "-23%" -> 23
        d.currency = priceText.find("R$") != std::string::npos ? L"R$" : priceText.find("\xE2\x82\xAC") != std::string::npos ? L"€"
                     : priceText.find('$') != std::string::npos ? L"US$" : L"";
        if (d.discount > 0 && d.discount < 100) d.oldPrice = d.price / (1.0 - d.discount / 100.0);
        // "Title - PC (Steam)": keep the store/platform part as subtitle
        size_t dash = d.title.rfind(L" - ");
        if (dash != std::wstring::npos) {
            d.platform = d.title.substr(dash + 3);
            d.title = d.title.substr(0, dash);
        }
        if (d.title.empty() || d.price <= 0 || d.discount <= 0 || IsNonGame(d.title) || !seen.insert(d.url).second) continue;
        out.push_back(std::move(d));
    }
    return out;
}

// Driffle: Next.js page data (prices in EUR, converted with the ECB reference rate).
std::vector<Deal> ParseDriffle(const std::string& html, double eurBrl) {
    std::vector<Deal> out;
    std::string data = Between(html, 0, "<script id=\"__NEXT_DATA__\" type=\"application/json\">", "</script>", html.size());
    json::Value root;
    if (data.empty() || !json::Parse(data, root)) return out;
    const json::Value& col = root["props"]["pageProps"]["mainCollection"];
    std::set<std::wstring> seen;
    for (const char* key : {"homepage_premium_deals", "homepage_best_selling_games", "homepage_latest_released_games",
                            "homepage_recently_added_games"}) {
        for (const auto& p : col[key].items()) {
            if (p["productType"].str() != "game") continue;
            double price = p["price"].asNumber(), mrp = p["mrp"].asNumber();
            if (price <= 0 || mrp <= price) continue;
            Deal d;
            d.title = CleanTitle(Widen(p["title"].str()));
            d.url = L"https://driffle.com/pt/" + Widen(p["slug"].str());
            d.image = Widen(p["coverImageOriginal"].str(p["image"].str()));
            std::wstring region = Widen(p["regionName"].str());
            std::wstring store = Widen(p["platform"].str());
            d.platform = store + (region.empty() ? L"" : L" · " + region);
            // "Killing Floor 2 - Steam": the platform is already shown below the title
            std::wstring suffix = L" - " + store;
            if (!store.empty() && d.title.size() > suffix.size() &&
                d.title.compare(d.title.size() - suffix.size(), suffix.size(), suffix) == 0)
                d.title.erase(d.title.size() - suffix.size());
            d.discount = static_cast<int>(std::lround(100.0 * (1.0 - price / mrp)));
            if (eurBrl > 0) {
                d.price = price * eurBrl;
                d.oldPrice = mrp * eurBrl;
                d.currency = L"R$";
                d.approx = true;
            } else {
                d.price = price;
                d.oldPrice = mrp;
                d.currency = L"€";
            }
            // The region tag inside the title is already in the subtitle
            if (!region.empty()) {
                std::wstring tag = L" (" + region + L")";
                size_t pos = d.title.find(tag);
                if (pos != std::wstring::npos) d.title.erase(pos, tag.size());
            }
            if (d.title.empty() || d.discount < 5 || IsNonGame(d.title) || !seen.insert(d.url).second) continue;
            out.push_back(std::move(d));
        }
    }
    return out;
}
}  // namespace

DealsService::DealsService() {
    auto add = [&](const char* id, const wchar_t* name, const wchar_t* url) {
        StoreDeals s;
        s.id = id;
        s.name = name;
        s.homeUrl = url;
        stores_.push_back(std::move(s));
    };
    add("gmg", L"Green Man Gaming", L"https://www.greenmangaming.com/pt/");
    add("ig", L"Instant Gaming", L"https://www.instant-gaming.com/br/");
    add("driffle", L"Driffle", L"https://driffle.com/pt");
    add("nuuvem", L"Nuuvem", L"https://www.nuuvem.com/br-pt");
    add("eneba", L"Eneba", L"https://www.eneba.com/");
    for (auto& s : stores_) {
        if (s.id == "nuuvem" || s.id == "eneba") {
            s.state = StoreDeals::State::Unsupported;
            s.message = s.id == "nuuvem" ? L"A Nuuvem bloqueia a leitura automática dos preços."
                                         : L"A Eneba só mostra os preços dentro do navegador.";
        }
    }
}

void DealsService::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& s : stores_)
            if (s.state != StoreDeals::State::Unsupported) LoadCache(s, INT64_MAX);  // show something immediately
        changed_ = true;
    }
    thread_ = std::thread([this] { Run(); });
}

void DealsService::Stop() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DealsService::SetOnline(bool on) {
    std::lock_guard<std::mutex> lk(m_);
    online_ = on;
}

void DealsService::Refresh(bool force) {
    {
        std::lock_guard<std::mutex> lk(m_);
        pending_ = true;
        force_ = force_ || force;
    }
    cv_.notify_one();
}

bool DealsService::Poll(std::vector<StoreDeals>& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (!changed_) return false;
    out = stores_;
    changed_ = false;
    return true;
}

bool DealsService::LoadCache(StoreDeals& s, int64_t maxAge) {
    std::wstring path = CachePath(s.id);
    int64_t mtime = FileModifiedUnix(path);
    std::string text;
    if (!mtime || NowUnix() - mtime > maxAge || !ReadFileBytes(path, text)) return false;
    json::Value v;
    if (!json::Parse(text, v)) return false;
    s.deals.clear();
    for (const auto& o : v["deals"].items()) {
        Deal d;
        d.title = Widen(o["title"].str());
        d.url = Widen(o["url"].str());
        d.image = Widen(o["image"].str());
        d.platform = Widen(o["platform"].str());
        d.currency = Widen(o["currency"].str());
        d.price = o["price"].asNumber();
        d.oldPrice = o["oldPrice"].asNumber();
        d.discount = static_cast<int>(o["discount"].asInt());
        d.approx = o["approx"].asBool();
        s.deals.push_back(std::move(d));
    }
    s.fetchedAt = mtime;
    s.state = StoreDeals::State::Ready;
    return true;
}

void DealsService::SaveCache(const StoreDeals& s) {
    json::Value v = json::Value::Object();
    json::Value arr = json::Value::Array();
    for (const auto& d : s.deals) {
        json::Value o = json::Value::Object();
        o.set("title", Narrow(d.title));
        o.set("url", Narrow(d.url));
        o.set("image", Narrow(d.image));
        o.set("platform", Narrow(d.platform));
        o.set("currency", Narrow(d.currency));
        o.set("price", d.price);
        o.set("oldPrice", d.oldPrice);
        o.set("discount", d.discount);
        o.set("approx", d.approx);
        arr.push(std::move(o));
    }
    v.set("deals", std::move(arr));
    WriteFileAtomic(CachePath(s.id), json::Serialize(v));
}

double DealsService::EurToBrl() {
    // European Central Bank daily reference rates (official, free, updated once a day).
    if (eurBrl_ > 0 && NowUnix() - eurFetchedAt_ < 12 * 3600) return eurBrl_;
    HttpResponse r = HttpGet(L"https://www.ecb.europa.eu/stats/eurofxref/eurofxref-daily.xml", 15000, 64 * 1024);
    if (r.ok()) {
        std::string rate = Between(r.body, 0, "currency='BRL' rate='", "'", r.body.size());
        double v = std::atof(rate.c_str());
        if (v > 0) {
            eurBrl_ = v;
            eurFetchedAt_ = NowUnix();
        }
    }
    return eurBrl_;
}

void DealsService::FetchStore(StoreDeals& s, bool force) {
    if (!force && LoadCache(s, kCacheSeconds)) return;
    std::wstring url = s.homeUrl;
    HttpResponse r = HttpGet(url, 25000, 8u << 20);
    std::vector<Deal> deals;
    if (r.ok()) {
        if (s.id == "gmg") deals = ParseGmg(r.body);
        else if (s.id == "ig") deals = ParseInstantGaming(r.body);
        else if (s.id == "driffle") deals = ParseDriffle(r.body, EurToBrl());
    }
    std::lock_guard<std::mutex> lk(m_);
    if (!deals.empty()) {
        std::stable_sort(deals.begin(), deals.end(), [](const Deal& a, const Deal& b) { return a.discount > b.discount; });
        s.deals = std::move(deals);
        s.state = StoreDeals::State::Ready;
        s.message.clear();
        s.fetchedAt = NowUnix();
        SaveCache(s);
    } else if (s.deals.empty()) {
        // Keep the last unreadable page for diagnosis (overwritten each time, never sent anywhere).
        if (r.ok()) WriteFileAtomic(JoinPath(AppSubDir(L"cache"), Widen("deals_" + s.id + "_unreadable.html")), r.body);
        s.state = StoreDeals::State::Error;
        s.message = r.status == 403 || r.status == 429 ? L"A loja recusou o acesso agora. Tente mais tarde."
                    : r.ok()                           ? L"A página da loja mudou e não foi possível ler as ofertas."
                                                       : L"Sem conexão com a loja.";
    }
    changed_ = true;
}

void DealsService::Run() {
    for (;;) {
        bool force;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || pending_; });
            if (stop_) return;
            pending_ = false;
            force = force_;
            force_ = false;
            if (!online_) continue;
            for (auto& s : stores_)
                if (s.state != StoreDeals::State::Unsupported && (force || s.deals.empty())) {
                    s.state = s.deals.empty() ? StoreDeals::State::Loading : s.state;
                    changed_ = true;
                }
        }
        for (size_t i = 0; i < stores_.size() && !stop_; ++i) {
            StoreDeals copy;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (stores_[i].state == StoreDeals::State::Unsupported) continue;
                copy = stores_[i];
            }
            FetchStore(copy, force);
            std::lock_guard<std::mutex> lk(m_);
            stores_[i] = std::move(copy);
            changed_ = true;
        }
    }
}

}  // namespace lumen
