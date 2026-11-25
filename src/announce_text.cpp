#include "announce_text.h"

#include "log.h"
#include "text_util.h"

#include <cstdio>
#include <cstring>
#include <cwctype>

namespace ep {
namespace {

// {D} = destination city, {W} = wish (day 5-18 h, evening 18-22 h, night). Languages with cases name the city after a
// noun ("destination, Kiel"), so the game's city name (nominative) always fits.
struct Text {
    const wchar_t* key;      // base language or base_region
    const wchar_t* withCity;
    const wchar_t* noCity;
    const wchar_t* wish[3];
};

const Text kTexts[] = {
    {L"de", L"Sehr geehrte Fahrgäste, wir erreichen in wenigen Minuten {D}. Wir bedanken uns für Ihre Reise und wünschen Ihnen {W}.",
     L"Sehr geehrte Fahrgäste, wir erreichen in wenigen Minuten unser Ziel. Wir bedanken uns für Ihre Reise und wünschen Ihnen {W}.",
     {L"einen angenehmen Tag", L"einen angenehmen Abend", L"eine gute Nacht"}},
    {L"en", L"Ladies and gentlemen, we will arrive in {D} in a few minutes. Thank you for travelling with us, and have {W}.",
     L"Ladies and gentlemen, we will arrive at our destination in a few minutes. Thank you for travelling with us, and have {W}.",
     {L"a pleasant day", L"a pleasant evening", L"a good night"}},
    {L"en_us", L"Ladies and gentlemen, we'll be arriving in {D} in a few minutes. Thank you for traveling with us, and have {W}.",
     L"Ladies and gentlemen, we'll be arriving at our destination in a few minutes. Thank you for traveling with us, and have {W}.",
     {L"a great day", L"a great evening", L"a good night"}},
    {L"fr", L"Mesdames et messieurs, nous arriverons à {D} dans quelques minutes. Merci d'avoir voyagé avec nous, nous vous souhaitons {W}.",
     L"Mesdames et messieurs, nous arriverons à destination dans quelques minutes. Merci d'avoir voyagé avec nous, nous vous souhaitons {W}.",
     {L"une agréable journée", L"une agréable soirée", L"une bonne nuit"}},
    {L"es", L"Señoras y señores, en unos minutos llegaremos a {D}. Gracias por viajar con nosotros, les deseamos {W}.",
     L"Señoras y señores, en unos minutos llegaremos a nuestro destino. Gracias por viajar con nosotros, les deseamos {W}.",
     {L"un buen día", L"una agradable tarde", L"una buena noche"}},
    {L"es_la", L"Señoras y señores, en unos minutos llegaremos a {D}. Gracias por viajar con nosotros, les deseamos {W}.",
     L"Señoras y señores, en unos minutos llegaremos a nuestro destino. Gracias por viajar con nosotros, les deseamos {W}.",
     {L"un excelente día", L"una excelente noche", L"una buena noche"}},
    {L"it", L"Gentili passeggeri, tra pochi minuti arriveremo a {D}. Grazie per aver viaggiato con noi, vi auguriamo {W}.",
     L"Gentili passeggeri, tra pochi minuti arriveremo a destinazione. Grazie per aver viaggiato con noi, vi auguriamo {W}.",
     {L"una buona giornata", L"una buona serata", L"una buona notte"}},
    {L"pt", L"Caros passageiros, dentro de poucos minutos chegaremos a {D}. Obrigado por viajarem connosco, desejamos-lhes {W}.",
     L"Caros passageiros, dentro de poucos minutos chegaremos ao nosso destino. Obrigado por viajarem connosco, desejamos-lhes {W}.",
     {L"um bom dia", L"uma boa noite", L"uma boa noite de descanso"}},
    {L"pt_br", L"Senhoras e senhores, em poucos minutos chegaremos a {D}. Obrigado por viajar conosco, desejamos a vocês {W}.",
     L"Senhoras e senhores, em poucos minutos chegaremos ao nosso destino. Obrigado por viajar conosco, desejamos a vocês {W}.",
     {L"um ótimo dia", L"uma ótima noite", L"uma boa noite de descanso"}},
    {L"nl", L"Dames en heren, over enkele minuten komen we aan in {D}. Bedankt dat u met ons heeft gereisd, en we wensen u nog {W}.",
     L"Dames en heren, over enkele minuten komen we aan op onze bestemming. Bedankt dat u met ons heeft gereisd, en we wensen u nog {W}.",
     {L"een prettige dag", L"een prettige avond", L"een goede nacht"}},
    {L"da", L"Kære passagerer, om få minutter ankommer vi til {D}. Tak fordi I rejste med os, og vi ønsker jer {W}.",
     L"Kære passagerer, om få minutter ankommer vi til vores destination. Tak fordi I rejste med os, og vi ønsker jer {W}.",
     {L"en god dag", L"en god aften", L"en god nat"}},
    {L"no", L"Kjære passasjerer, om noen minutter ankommer vi til {D}. Takk for at dere reiste med oss, og vi ønsker dere {W}.",
     L"Kjære passasjerer, om noen minutter ankommer vi til bestemmelsesstedet. Takk for at dere reiste med oss, og vi ønsker dere {W}.",
     {L"en fin dag", L"en fin kveld", L"god natt"}},
    {L"sv", L"Bästa resenärer, om några minuter anländer vi till {D}. Tack för att ni reste med oss, och vi önskar er {W}.",
     L"Bästa resenärer, om några minuter anländer vi till vår destination. Tack för att ni reste med oss, och vi önskar er {W}.",
     {L"en trevlig dag", L"en trevlig kväll", L"en god natt"}},
    {L"fi", L"Hyvät matkustajat, saavumme muutaman minuutin kuluttua määränpäähämme, {D}. Kiitos, että matkustitte kanssamme. Toivotamme teille {W}.",
     L"Hyvät matkustajat, saavumme muutaman minuutin kuluttua määränpäähämme. Kiitos, että matkustitte kanssamme. Toivotamme teille {W}.",
     {L"hyvää päivänjatkoa", L"hyvää illanjatkoa", L"hyvää yötä"}},
    {L"et", L"Head reisijad, jõuame mõne minuti pärast sihtkohta, {D}. Täname, et reisisite meiega, ja soovime teile {W}.",
     L"Head reisijad, jõuame mõne minuti pärast sihtkohta. Täname, et reisisite meiega, ja soovime teile {W}.",
     {L"meeldivat päeva", L"meeldivat õhtut", L"head ööd"}},
    {L"lv", L"Godātie pasažieri, pēc dažām minūtēm mēs ieradīsimies galamērķī, {D}. Paldies, ka ceļojāt kopā ar mums, un novēlam jums {W}.",
     L"Godātie pasažieri, pēc dažām minūtēm mēs ieradīsimies galamērķī. Paldies, ka ceļojāt kopā ar mums, un novēlam jums {W}.",
     {L"patīkamu dienu", L"patīkamu vakaru", L"labu nakti"}},
    {L"lt", L"Gerbiami keleiviai, po kelių minučių atvyksime į kelionės tikslą, {D}. Dėkojame, kad keliavote su mumis, ir linkime jums {W}.",
     L"Gerbiami keleiviai, po kelių minučių atvyksime į kelionės tikslą. Dėkojame, kad keliavote su mumis, ir linkime jums {W}.",
     {L"malonios dienos", L"malonaus vakaro", L"geros nakties"}},
    {L"pl", L"Szanowni Państwo, za kilka minut dotrzemy do celu podróży, {D}. Dziękujemy za wspólną podróż i życzymy {W}.",
     L"Szanowni Państwo, za kilka minut dotrzemy do celu podróży. Dziękujemy za wspólną podróż i życzymy {W}.",
     {L"miłego dnia", L"miłego wieczoru", L"dobrej nocy"}},
    {L"cs", L"Vážení cestující, za několik minut dorazíme do cílové stanice {D}. Děkujeme, že jste cestovali s námi, a přejeme vám {W}.",
     L"Vážení cestující, za několik minut dorazíme do cílové stanice. Děkujeme, že jste cestovali s námi, a přejeme vám {W}.",
     {L"příjemný den", L"příjemný večer", L"dobrou noc"}},
    {L"sk", L"Vážení cestujúci, o niekoľko minút prídeme do cieľovej stanice {D}. Ďakujeme, že ste cestovali s nami, a želáme vám {W}.",
     L"Vážení cestujúci, o niekoľko minút prídeme do cieľovej stanice. Ďakujeme, že ste cestovali s nami, a želáme vám {W}.",
     {L"príjemný deň", L"príjemný večer", L"dobrú noc"}},
    {L"hu", L"Kedves Utasaink! Néhány perc múlva megérkezünk úti célunkhoz, {D}. Köszönjük, hogy velünk utaztak, és {W} kívánunk.",
     L"Kedves Utasaink! Néhány perc múlva megérkezünk úti célunkhoz. Köszönjük, hogy velünk utaztak, és {W} kívánunk.",
     {L"kellemes napot", L"kellemes estét", L"jó éjszakát"}},
    {L"ro", L"Stimați pasageri, în câteva minute ajungem la {D}. Vă mulțumim că ați călătorit cu noi și vă dorim {W}.",
     L"Stimați pasageri, în câteva minute ajungem la destinație. Vă mulțumim că ați călătorit cu noi și vă dorim {W}.",
     {L"o zi plăcută", L"o seară plăcută", L"noapte bună"}},
    {L"sl", L"Spoštovani potniki, čez nekaj minut prispemo na cilj, {D}. Hvala, ker ste potovali z nami, in želimo vam {W}.",
     L"Spoštovani potniki, čez nekaj minut prispemo na cilj. Hvala, ker ste potovali z nami, in želimo vam {W}.",
     {L"prijeten dan", L"prijeten večer", L"lahko noč"}},
    {L"hr", L"Poštovani putnici, za nekoliko minuta stižemo na odredište, {D}. Hvala što ste putovali s nama i želimo vam {W}.",
     L"Poštovani putnici, za nekoliko minuta stižemo na odredište. Hvala što ste putovali s nama i želimo vam {W}.",
     {L"ugodan dan", L"ugodnu večer", L"laku noć"}},
    {L"sr", L"Поштовани путници, за неколико минута стижемо на одредиште, {D}. Хвала што сте путовали са нама и желимо вам {W}.",
     L"Поштовани путници, за неколико минута стижемо на одредиште. Хвала што сте путовали са нама и желимо вам {W}.",
     {L"пријатан дан", L"пријатно вече", L"лаку ноћ"}},
    {L"bg", L"Уважаеми пътници, след няколко минути пристигаме в {D}. Благодарим ви, че пътувахте с нас, и ви пожелаваме {W}.",
     L"Уважаеми пътници, след няколко минути пристигаме в крайната точка. Благодарим ви, че пътувахте с нас, и ви пожелаваме {W}.",
     {L"приятен ден", L"приятна вечер", L"лека нощ"}},
    {L"mk", L"Почитувани патници, за неколку минути пристигнуваме на крајната дестинација, {D}. Ви благодариме што патувавте со нас и ви посакуваме {W}.",
     L"Почитувани патници, за неколку минути пристигнуваме на крајната дестинација. Ви благодариме што патувавте со нас и ви посакуваме {W}.",
     {L"пријатен ден", L"пријатна вечер", L"добра ноќ"}},
    {L"ru", L"Уважаемые пассажиры, через несколько минут мы прибываем в пункт назначения, {D}. Благодарим вас за поездку и желаем вам {W}.",
     L"Уважаемые пассажиры, через несколько минут мы прибываем в пункт назначения. Благодарим вас за поездку и желаем вам {W}.",
     {L"приятного дня", L"приятного вечера", L"спокойной ночи"}},
    {L"uk", L"Шановні пасажири, за кілька хвилин ми прибуваємо до пункту призначення, {D}. Дякуємо, що подорожували з нами, і бажаємо вам {W}.",
     L"Шановні пасажири, за кілька хвилин ми прибуваємо до пункту призначення. Дякуємо, що подорожували з нами, і бажаємо вам {W}.",
     {L"приємного дня", L"приємного вечора", L"доброї ночі"}},
    {L"el", L"Αγαπητοί επιβάτες, σε λίγα λεπτά φτάνουμε στον προορισμό μας, {D}. Σας ευχαριστούμε που ταξιδέψατε μαζί μας και σας ευχόμαστε {W}.",
     L"Αγαπητοί επιβάτες, σε λίγα λεπτά φτάνουμε στον προορισμό μας. Σας ευχαριστούμε που ταξιδέψατε μαζί μας και σας ευχόμαστε {W}.",
     {L"καλή συνέχεια στη μέρα σας", L"καλό βράδυ", L"καληνύχτα"}},
    {L"tr", L"Sayın yolcularımız, birkaç dakika içinde varış noktamıza ulaşıyoruz, {D}. Bizimle seyahat ettiğiniz için teşekkür eder, {W} dileriz.",
     L"Sayın yolcularımız, birkaç dakika içinde varış noktamıza ulaşıyoruz. Bizimle seyahat ettiğiniz için teşekkür eder, {W} dileriz.",
     {L"iyi günler", L"iyi akşamlar", L"iyi geceler"}},
    {L"ca", L"Senyores i senyors, d'aquí a uns minuts arribarem a {D}. Gràcies per viatjar amb nosaltres, us desitgem {W}.",
     L"Senyores i senyors, d'aquí a uns minuts arribarem a la nostra destinació. Gràcies per viatjar amb nosaltres, us desitgem {W}.",
     {L"un bon dia", L"un bon vespre", L"una bona nit"}},
    {L"gl", L"Señoras e señores, en poucos minutos chegaremos a {D}. Grazas por viaxar connosco, desexámoslles {W}.",
     L"Señoras e señores, en poucos minutos chegaremos ao noso destino. Grazas por viaxar connosco, desexámoslles {W}.",
     {L"un bo día", L"unha boa tarde", L"unha boa noite"}},
    {L"eu", L"Bidaiari agurgarriak, minutu gutxi barru helmugara iritsiko gara, {D}. Eskerrik asko gurekin bidaiatzeagatik, eta {W} opa dizuegu.",
     L"Bidaiari agurgarriak, minutu gutxi barru helmugara iritsiko gara. Eskerrik asko gurekin bidaiatzeagatik, eta {W} opa dizuegu.",
     {L"egun ona", L"arratsalde ona", L"gau ona"}},
    {L"ka", L"ძვირფასო მგზავრებო, რამდენიმე წუთში მივაღწევთ დანიშნულების ადგილს, {D}. გმადლობთ, რომ ჩვენთან ერთად იმოგზაურეთ, და გისურვებთ {W}.",
     L"ძვირფასო მგზავრებო, რამდენიმე წუთში მივაღწევთ დანიშნულების ადგილს. გმადლობთ, რომ ჩვენთან ერთად იმოგზაურეთ, და გისურვებთ {W}.",
     {L"სასიამოვნო დღეს", L"სასიამოვნო საღამოს", L"მშვიდ ღამეს"}},
    {L"ko", L"승객 여러분, 잠시 후 목적지인 {D}에 도착합니다. 저희와 함께 여행해 주셔서 감사합니다. {W}.",
     L"승객 여러분, 잠시 후 목적지에 도착합니다. 저희와 함께 여행해 주셔서 감사합니다. {W}.",
     {L"좋은 하루 보내세요", L"즐거운 저녁 보내세요", L"편안한 밤 보내세요"}},
    {L"ja", L"皆様、まもなく{D}に到着いたします。ご乗車ありがとうございました。{W}。",
     L"皆様、まもなく目的地に到着いたします。ご乗車ありがとうございました。{W}。",
     {L"良い一日をお過ごしください", L"素敵な夜をお過ごしください", L"おやすみなさいませ"}},
    {L"zh", L"各位乘客，我们将在几分钟后到达{D}。感谢您与我们同行，祝您{W}。",
     L"各位乘客，我们将在几分钟后到达目的地。感谢您与我们同行，祝您{W}。",
     {L"一天愉快", L"晚上愉快", L"晚安"}},
    {L"zh_tw", L"各位乘客，我們將在幾分鐘後抵達{D}。感謝您與我們同行，祝您{W}。",
     L"各位乘客，我們將在幾分鐘後抵達目的地。感謝您與我們同行，祝您{W}。",
     {L"一天愉快", L"晚上愉快", L"晚安"}},
    {L"vi", L"Kính thưa quý khách, chỉ ít phút nữa chúng ta sẽ đến {D}. Cảm ơn quý khách đã đồng hành cùng chúng tôi, chúc quý khách {W}.",
     L"Kính thưa quý khách, chỉ ít phút nữa chúng ta sẽ đến nơi. Cảm ơn quý khách đã đồng hành cùng chúng tôi, chúc quý khách {W}.",
     {L"một ngày vui vẻ", L"một buổi tối vui vẻ", L"ngủ ngon"}},
};

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

AnnounceLang Parse(std::wstring code) {
    code = Lower(code);
    for (auto& c : code) if (c == L'-') c = L'_';
    AnnounceLang l;
    const size_t u = code.find(L'_');
    l.base = code.substr(0, u);
    if (u != std::wstring::npos) l.region = code.substr(u + 1);
    if (l.base == L"nb" || l.base == L"nn") l.base = L"no";
    if (l.base == L"es" && l.region == L"mx") l.region = L"la";
    return l;
}

// config.cfg: uset g_lang "fr_fr" (language chosen in the game) - older/fresh installs only have g_lang_init.
AnnounceLang GameLanguage() {
    std::string cfg;
    if (FILE* f = _wfopen((GameDocsDir() + L"\\config.cfg").c_str(), L"rb")) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) cfg.append(buf, n);
        fclose(f);
    }
    for (const char* key : {"uset g_lang \"", "uset g_lang_init \""}) {
        const size_t p = cfg.find(key);
        if (p == std::string::npos) continue;
        const size_t s = p + strlen(key), e = cfg.find('"', s);
        if (e != std::string::npos && e > s) return Parse(ToWide(cfg.substr(s, e - s)));
    }
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) return Parse(name);
    return Parse(L"en_gb");
}

const Text* Find(const AnnounceLang& l) {
    const std::wstring code = l.Code();
    for (const Text& t : kTexts) if (code == t.key) return &t;
    for (const Text& t : kTexts) if (l.base == t.key) return &t;
    return nullptr;
}

void Replace(std::wstring& s, const wchar_t* what, const std::wstring& with) {
    const size_t p = s.find(what);
    if (p != std::wstring::npos) s.replace(p, wcslen(what), with);
}

}  // namespace

AnnounceLang CurrentGameLanguage() { return GameLanguage(); }

bool HaveAnnounceText(const std::wstring& base) {
    AnnounceLang l;
    l.base = base;
    return Find(l) != nullptr;
}

std::vector<AnnounceLang> AnnounceLanguages(const std::wstring& setting) {
    std::vector<AnnounceLang> out;
    size_t s = 0;
    while (s <= setting.size()) {
        size_t e = setting.find(L',', s);
        if (e == std::wstring::npos) e = setting.size();
        std::wstring item = setting.substr(s, e - s);
        while (!item.empty() && iswspace(item.front())) item.erase(item.begin());
        while (!item.empty() && iswspace(item.back())) item.pop_back();
        if (!item.empty()) {
            AnnounceLang l = Lower(item) == L"auto" ? GameLanguage() : Parse(item);
            if (!Find(l)) {
                Logf("ANNOUNCE no text for language '%ls', English is used", l.Code().c_str());
                l = Parse(L"en_gb");
            }
            bool dup = false;
            for (const auto& o : out) dup = dup || o.Code() == l.Code();
            if (!dup) out.push_back(l);
        }
        s = e + 1;
    }
    if (out.empty()) out.push_back(GameLanguage());
    if (!Find(out[0])) out[0] = Parse(L"en_gb");
    return out;
}

// Piper voice for a language: voice\<base>_<REGION>-<name>-<quality>.onnx (e.g. de_DE-thorsten-high.onnx). Same region
// first (en_gb -> en_GB, es_la -> es_MX), then the usual one of the language, then any of the language. "" = none.
std::wstring PiperVoiceFile(const std::wstring& dir, const AnnounceLang& lang) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\voice\\*.onnx").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do files.push_back(fd.cFileName); while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    auto lower = [](std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; };
    const std::wstring region = lang.region == L"la" ? L"mx" : lang.region;
    const std::wstring usual = lang.base == L"en" ? L"gb" : lang.base == L"zh" ? L"cn" : lang.base;
    std::vector<std::wstring> prefixes;
    if (!region.empty()) prefixes.push_back(lang.base + L"_" + region + L"-");
    prefixes.push_back(lang.base + L"_" + usual + L"-");
    prefixes.push_back(lang.base + L"_");
    for (const auto& prefix : prefixes)
        for (const auto& name : files)
            if (lower(name).rfind(prefix, 0) == 0) return dir + L"\\voice\\" + name;
    return L"";
}

std::wstring AnnounceText(const AnnounceLang& lang, const std::wstring& dest, int hour) {
    const Text* t = Find(lang);
    if (!t) t = &kTexts[1];
    const int w = hour >= 5 && hour < 18 ? 0 : hour >= 18 && hour < 22 ? 1 : 2;
    std::wstring s = dest.empty() ? t->noCity : t->withCity;
    Replace(s, L"{D}", dest);
    Replace(s, L"{W}", t->wish[w]);
    return s;
}

}  // namespace ep
