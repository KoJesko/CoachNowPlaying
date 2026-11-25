#include "tv_text.h"

#include <vector>

namespace ep {
namespace {

// key | destination | welcome | arrival | next stop | destination (no city) | belongings | good trip | seat belt |
// wi-fi + toilet | Monday .. Sunday
const wchar_t* kTv[] = {
    L"en|Destination|Welcome on board|Arrival approx. {T}  ·  {K} km to go|Next stop|Destination|Please take all your belongings with you.|Have a good trip!|Please keep your seat belt fastened.|Free Wi-Fi  ·  Toilet at the rear|Monday|Tuesday|Wednesday|Thursday|Friday|Saturday|Sunday",
    L"en_us|Destination|Welcome aboard|Arrival approx. {T}  ·  {K} km to go|Next stop|Destination|Please take all your belongings with you.|Have a great trip!|Please keep your seat belt fastened.|Free Wi-Fi  ·  Restroom in the back|Monday|Tuesday|Wednesday|Thursday|Friday|Saturday|Sunday",
    L"de|Fahrtziel|Willkommen an Bord|Ankunft ca. {T}  ·  noch {K} km|Nächster Halt|Ziel|Bitte nichts im Bus vergessen.|Gute Fahrt!|Bitte während der Fahrt angeschnallt bleiben.|Kostenloses WLAN  ·  WC im Heck|Montag|Dienstag|Mittwoch|Donnerstag|Freitag|Samstag|Sonntag",
    L"fr|Destination|Bienvenue à bord|Arrivée vers {T}  ·  encore {K} km|Prochain arrêt|Destination|N'oubliez rien dans le car.|Bon voyage !|Veuillez garder votre ceinture attachée.|Wi-Fi gratuit  ·  Toilettes à l'arrière|Lundi|Mardi|Mercredi|Jeudi|Vendredi|Samedi|Dimanche",
    L"es|Destino|Bienvenidos a bordo|Llegada aprox. {T}  ·  faltan {K} km|Próxima parada|Destino|No olviden sus pertenencias.|¡Buen viaje!|Mantengan el cinturón abrochado.|Wi-Fi gratis  ·  Aseo al fondo|Lunes|Martes|Miércoles|Jueves|Viernes|Sábado|Domingo",
    L"es_la|Destino|Bienvenidos a bordo|Llegada aprox. {T}  ·  faltan {K} km|Próxima parada|Destino|No olviden sus pertenencias.|¡Buen viaje!|Mantengan el cinturón abrochado.|Wi-Fi gratis  ·  Baño al fondo|Lunes|Martes|Miércoles|Jueves|Viernes|Sábado|Domingo",
    L"it|Destinazione|Benvenuti a bordo|Arrivo previsto {T}  ·  ancora {K} km|Prossima fermata|Destinazione|Non dimenticate nulla a bordo.|Buon viaggio!|Tenete allacciate le cinture di sicurezza.|Wi-Fi gratuito  ·  WC in fondo|Lunedì|Martedì|Mercoledì|Giovedì|Venerdì|Sabato|Domenica",
    L"pt|Destino|Bem-vindos a bordo|Chegada prevista {T}  ·  faltam {K} km|Próxima paragem|Destino|Não se esqueça dos seus pertences.|Boa viagem!|Mantenha o cinto de segurança apertado.|Wi-Fi grátis  ·  WC na traseira|Segunda-feira|Terça-feira|Quarta-feira|Quinta-feira|Sexta-feira|Sábado|Domingo",
    L"pt_br|Destino|Bem-vindos a bordo|Chegada prevista {T}  ·  faltam {K} km|Próxima parada|Destino|Não esqueça seus pertences.|Boa viagem!|Mantenha o cinto de segurança afivelado.|Wi-Fi grátis  ·  Banheiro no fundo|Segunda-feira|Terça-feira|Quarta-feira|Quinta-feira|Sexta-feira|Sábado|Domingo",
    L"nl|Bestemming|Welkom aan boord|Aankomst ca. {T}  ·  nog {K} km|Volgende halte|Bestemming|Vergeet uw spullen niet.|Goede reis!|Houd tijdens de rit uw gordel om.|Gratis wifi  ·  Toilet achterin|Maandag|Dinsdag|Woensdag|Donderdag|Vrijdag|Zaterdag|Zondag",
    L"da|Destination|Velkommen om bord|Ankomst ca. {T}  ·  {K} km tilbage|Næste stop|Destination|Husk at tage alle dine ejendele med.|God tur!|Hold sikkerhedsselen spændt under kørslen.|Gratis wifi  ·  Toilet bagerst|Mandag|Tirsdag|Onsdag|Torsdag|Fredag|Lørdag|Søndag",
    L"no|Reisemål|Velkommen om bord|Ankomst ca. {T}  ·  {K} km igjen|Neste stopp|Reisemål|Husk å ta med deg alle eiendeler.|God tur!|Ha setebeltet på under kjøringen.|Gratis Wi-Fi  ·  Toalett bak|Mandag|Tirsdag|Onsdag|Torsdag|Fredag|Lørdag|Søndag",
    L"sv|Destination|Välkommen ombord|Ankomst ca {T}  ·  {K} km kvar|Nästa hållplats|Destination|Glöm inte dina tillhörigheter.|Trevlig resa!|Ha bältet fastspänt under resan.|Gratis wifi  ·  Toalett längst bak|Måndag|Tisdag|Onsdag|Torsdag|Fredag|Lördag|Söndag",
    L"fi|Määränpää|Tervetuloa kyytiin|Perillä n. {T}  ·  {K} km jäljellä|Seuraava pysäkki|Määränpää|Muistathan ottaa kaikki tavarasi mukaan.|Hyvää matkaa!|Pidä turvavyö kiinni matkan aikana.|Ilmainen Wi-Fi  ·  WC takana|Maanantai|Tiistai|Keskiviikko|Torstai|Perjantai|Lauantai|Sunnuntai",
    L"et|Sihtkoht|Tere tulemast pardale|Saabumine u {T}  ·  veel {K} km|Järgmine peatus|Sihtkoht|Ärge unustage oma asju.|Head reisi!|Hoidke turvavöö sõidu ajal kinni.|Tasuta WiFi  ·  WC tagaosas|Esmaspäev|Teisipäev|Kolmapäev|Neljapäev|Reede|Laupäev|Pühapäev",
    L"lv|Galamērķis|Laipni lūdzam|Ierašanās ap {T}  ·  vēl {K} km|Nākamā pietura|Galamērķis|Neaizmirstiet savas mantas.|Laimīgu ceļu!|Brauciena laikā turiet piesprādzētu drošības jostu.|Bezmaksas Wi-Fi  ·  Tualete aizmugurē|Pirmdiena|Otrdiena|Trešdiena|Ceturtdiena|Piektdiena|Sestdiena|Svētdiena",
    L"lt|Kelionės tikslas|Sveiki atvykę|Atvykimas apie {T}  ·  liko {K} km|Kita stotelė|Kelionės tikslas|Nepamirškite savo daiktų.|Geros kelionės!|Kelionės metu būkite prisisegę saugos diržą.|Nemokamas Wi-Fi  ·  Tualetas gale|Pirmadienis|Antradienis|Trečiadienis|Ketvirtadienis|Penktadienis|Šeštadienis|Sekmadienis",
    L"pl|Cel podróży|Witamy na pokładzie|Przyjazd ok. {T}  ·  jeszcze {K} km|Następny przystanek|Cel podróży|Prosimy nie zostawiać rzeczy w autokarze.|Szerokiej drogi!|Prosimy mieć zapięte pasy podczas jazdy.|Bezpłatne Wi-Fi  ·  WC z tyłu|Poniedziałek|Wtorek|Środa|Czwartek|Piątek|Sobota|Niedziela",
    L"cs|Cíl cesty|Vítejte na palubě|Příjezd cca {T}  ·  zbývá {K} km|Příští zastávka|Cíl cesty|Nezapomeňte si své věci.|Šťastnou cestu!|Během jízdy mějte prosím zapnutý pás.|Wi-Fi zdarma  ·  WC vzadu|Pondělí|Úterý|Středa|Čtvrtek|Pátek|Sobota|Neděle",
    L"sk|Cieľ cesty|Vitajte na palube|Príchod cca {T}  ·  zostáva {K} km|Ďalšia zastávka|Cieľ cesty|Nezabudnite si svoje veci.|Šťastnú cestu!|Počas jazdy majte zapnutý bezpečnostný pás.|Wi-Fi zadarmo  ·  WC vzadu|Pondelok|Utorok|Streda|Štvrtok|Piatok|Sobota|Nedeľa",
    L"hu|Úti cél|Üdvözöljük a fedélzeten|Érkezés kb. {T}  ·  még {K} km|Következő megálló|Úti cél|Kérjük, ne hagyja itt csomagjait.|Jó utat!|Utazás közben tartsa bekapcsolva a biztonsági övet.|Ingyenes Wi-Fi  ·  Mosdó hátul|Hétfő|Kedd|Szerda|Csütörtök|Péntek|Szombat|Vasárnap",
    L"ro|Destinație|Bine ați venit la bord|Sosire aprox. {T}  ·  încă {K} km|Următoarea oprire|Destinație|Nu vă uitați bagajele.|Drum bun!|Vă rugăm să păstrați centura legată.|Wi-Fi gratuit  ·  Toaletă în spate|Luni|Marți|Miercuri|Joi|Vineri|Sâmbătă|Duminică",
    L"sl|Cilj|Dobrodošli na krovu|Prihod okoli {T}  ·  še {K} km|Naslednja postaja|Cilj|Ne pozabite svojih stvari.|Srečno pot!|Med vožnjo imejte pripet varnostni pas.|Brezplačen Wi-Fi  ·  WC zadaj|Ponedeljek|Torek|Sreda|Četrtek|Petek|Sobota|Nedelja",
    L"hr|Odredište|Dobro došli|Dolazak oko {T}  ·  još {K} km|Sljedeća stanica|Odredište|Ne zaboravite svoje stvari.|Sretan put!|Tijekom vožnje držite pojas vezanim.|Besplatni Wi-Fi  ·  WC straga|Ponedjeljak|Utorak|Srijeda|Četvrtak|Petak|Subota|Nedjelja",
    L"sr|Одредиште|Добро дошли|Долазак око {T}  ·  још {K} km|Следећа станица|Одредиште|Не заборавите своје ствари.|Срећан пут!|Током вожње држите појас везаним.|Бесплатан Wi-Fi  ·  WC позади|Понедељак|Уторак|Среда|Четвртак|Петак|Субота|Недеља",
    L"bg|Направление|Добре дошли|Пристигане около {T}  ·  още {K} km|Следваща спирка|Направление|Не забравяйте багажа си.|Приятно пътуване!|Моля, пазете колана закопчан.|Безплатен Wi-Fi  ·  Тоалетна отзад|Понеделник|Вторник|Сряда|Четвъртък|Петък|Събота|Неделя",
    L"mk|Дестинација|Добредојдовте|Пристигнување околу {T}  ·  уште {K} km|Следна станица|Дестинација|Не ги заборавајте вашите работи.|Среќен пат!|Ве молиме држете го појасот врзан.|Бесплатен Wi-Fi  ·  Тоалет позади|Понеделник|Вторник|Среда|Четврток|Петок|Сабота|Недела",
    L"ru|Пункт назначения|Добро пожаловать на борт|Прибытие ок. {T}  ·  ещё {K} км|Следующая остановка|Пункт назначения|Не забывайте свои вещи.|Счастливого пути!|Пожалуйста, не отстёгивайте ремень во время поездки.|Бесплатный Wi-Fi  ·  Туалет в конце салона|Понедельник|Вторник|Среда|Четверг|Пятница|Суббота|Воскресенье",
    L"uk|Пункт призначення|Ласкаво просимо на борт|Прибуття близько {T}  ·  ще {K} км|Наступна зупинка|Пункт призначення|Не забувайте свої речі.|Щасливої дороги!|Будь ласка, не відстібайте пасок під час поїздки.|Безкоштовний Wi-Fi  ·  Туалет у кінці салону|Понеділок|Вівторок|Середа|Четвер|Пʼятниця|Субота|Неділя",
    L"el|Προορισμός|Καλώς ήρθατε|Άφιξη περίπου {T}  ·  απομένουν {K} km|Επόμενη στάση|Προορισμός|Μην ξεχάσετε τα πράγματά σας.|Καλό ταξίδι!|Παρακαλούμε κρατήστε τη ζώνη δεμένη.|Δωρεάν Wi-Fi  ·  Τουαλέτα πίσω|Δευτέρα|Τρίτη|Τετάρτη|Πέμπτη|Παρασκευή|Σάββατο|Κυριακή",
    L"tr|Varış noktası|Hoş geldiniz|Tahmini varış {T}  ·  {K} km kaldı|Sonraki durak|Varış noktası|Lütfen eşyalarınızı unutmayın.|İyi yolculuklar!|Yolculuk boyunca emniyet kemerinizi takılı tutun.|Ücretsiz Wi-Fi  ·  Tuvalet arkada|Pazartesi|Salı|Çarşamba|Perşembe|Cuma|Cumartesi|Pazar",
    L"ca|Destinació|Benvinguts a bord|Arribada aprox. {T}  ·  falten {K} km|Propera parada|Destinació|No oblideu les vostres coses.|Bon viatge!|Mantingueu el cinturó cordat.|Wi-Fi gratuït  ·  Lavabo al fons|Dilluns|Dimarts|Dimecres|Dijous|Divendres|Dissabte|Diumenge",
    L"gl|Destino|Benvidos a bordo|Chegada aprox. {T}  ·  faltan {K} km|Próxima parada|Destino|Non esquezan as súas cousas.|Boa viaxe!|Manteñan o cinto abrochado.|Wi-Fi gratuíto  ·  Aseo ao fondo|Luns|Martes|Mércores|Xoves|Venres|Sábado|Domingo",
    L"eu|Helmuga|Ongi etorri|Iritsiera {T} inguruan  ·  {K} km falta|Hurrengo geltokia|Helmuga|Ez ahaztu zuen gauzak.|Bidaia ona!|Mesedez, eutsi segurtasun-uhala lotuta.|Doako Wi-Fi  ·  Komuna atzean|Astelehena|Asteartea|Asteazkena|Osteguna|Ostirala|Larunbata|Igandea",
    L"ka|დანიშნულება|კეთილი იყოს თქვენი მობრძანება|ჩასვლა დაახლ. {T}  ·  დარჩა {K} კმ|შემდეგი გაჩერება|დანიშნულება|ნუ დაგავიწყდებათ ნივთები.|ბედნიერ მგზავრობას!|მგზავრობისას შეიკარით ღვედი.|უფასო Wi-Fi  ·  ტუალეტი უკან|ორშაბათი|სამშაბათი|ოთხშაბათი|ხუთშაბათი|პარასკევი|შაბათი|კვირა",
    L"ko|목적지|탑승을 환영합니다|도착 예정 {T}  ·  남은 거리 {K} km|다음 정류장|목적지|소지품을 잊지 마세요.|즐거운 여행 되세요!|운행 중에는 안전벨트를 착용해 주세요.|무료 Wi-Fi  ·  화장실은 뒤쪽|월요일|화요일|수요일|목요일|금요일|토요일|일요일",
    L"ja|行き先|ご乗車ありがとうございます|到着予定 {T}  ·  残り {K} km|次の停留所|目的地|お忘れ物のないようご注意ください。|よい旅を！|走行中はシートベルトをお締めください。|無料Wi-Fi  ·  トイレは後方|月曜日|火曜日|水曜日|木曜日|金曜日|土曜日|日曜日",
    L"zh|目的地|欢迎乘车|预计到达 {T}  ·  剩余 {K} 公里|下一站|目的地|请带好您的随身物品。|旅途愉快！|行车途中请系好安全带。|免费Wi-Fi  ·  洗手间在后部|星期一|星期二|星期三|星期四|星期五|星期六|星期日",
    L"zh_tw|目的地|歡迎搭乘|預計抵達 {T}  ·  剩餘 {K} 公里|下一站|目的地|請帶好您的隨身物品。|旅途愉快！|行車途中請繫好安全帶。|免費Wi-Fi  ·  洗手間在後方|星期一|星期二|星期三|星期四|星期五|星期六|星期日",
    L"vi|Điểm đến|Chào mừng quý khách|Dự kiến đến {T}  ·  còn {K} km|Điểm dừng tiếp theo|Điểm đến|Xin đừng quên hành lý của quý khách.|Chúc thượng lộ bình an!|Vui lòng thắt dây an toàn trong suốt chuyến đi.|Wi-Fi miễn phí  ·  Nhà vệ sinh phía sau|Thứ Hai|Thứ Ba|Thứ Tư|Thứ Năm|Thứ Sáu|Thứ Bảy|Chủ Nhật",
};

std::vector<std::wstring> Split(const wchar_t* s) {
    std::vector<std::wstring> out(1);
    for (; *s; ++s) {
        if (*s == L'|') out.emplace_back();
        else out.back() += *s;
    }
    return out;
}

}  // namespace

TvText GetTvText(const AnnounceLang& lang) {
    const std::wstring code = lang.Code();
    const wchar_t* pick = nullptr;
    for (const std::wstring& want : {code, lang.base})
        for (const wchar_t* t : kTv)
            if (!pick && Split(t)[0] == want) pick = t;
    if (!pick) pick = kTv[0];
    const std::vector<std::wstring> f = Split(pick);
    TvText r;
    if (f.size() < 17) return GetTvText(AnnounceLang{L"en", L"gb"});
    r.english = f[0].rfind(L"en", 0) == 0;
    r.destination = f[1]; r.welcome = f[2]; r.arrival = f[3]; r.nextStop = f[4]; r.target = f[5];
    r.belongings = f[6]; r.goodTrip = f[7]; r.seatBelt = f[8]; r.wifi = f[9];
    for (int i = 0; i < 7; ++i) r.days[i] = f[10 + i];
    return r;
}

}  // namespace ep
