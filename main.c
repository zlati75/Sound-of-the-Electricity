/*
 * Sound of the Electricity – уеб сайт, написан на C (без JavaScript).
 * Програмата е малък HTTP сървър: сглобява HTML страниците и ги праща на браузъра.
 *
 * Компилация (Linux / GitHub Codespaces):
 *     gcc -pthread main.c -o shop
 *     ./shop            (по подразбиране порт 8080)
 *
 * Всичко е локално: няма външни шрифтове, скриптове или връзки към интернет.
 * Всички връзки между страниците са относителни (custom, cart, ./ ...),
 * затова работят и когато сайтът е зад прокси с префикс на адреса.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <netinet/in.h>

#include "style_css.h"

/* ---------- динамичен текстов буфер ---------- */
typedef struct { char *d; size_t n, cap; } Buf;

static void bcat(Buf *b, const char *fmt, ...) {
    va_list a, a2;
    va_start(a, fmt);
    va_copy(a2, a);
    int need = vsnprintf(NULL, 0, fmt, a);
    va_end(a);
    if (need < 0) { va_end(a2); return; }
    if (b->n + need + 1 > b->cap) {
        b->cap = (b->n + need + 1) * 2;
        b->d = realloc(b->d, b->cap);
    }
    vsnprintf(b->d + b->n, need + 1, fmt, a2);
    va_end(a2);
    b->n += need;
}

/* ---------- данни ---------- */
typedef struct { const char *id, *cat, *name, *desc; int price; } Product;
static const Product P[] = {
    {"g1","guitars","Voltage ST-60","Двойна изрезка, три сингъла, кленов гриф.",849},
    {"g2","guitars","Nightbird LP","Единична изрезка, два хъмбъкъра, дебел и топъл звук.",1099},
    {"g3","guitars","Prairie TL","Ярък, чист и режещ тон за кънтри и блус.",729},
    {"e1","effects","Overdrive Amber","Топъл овърдрайв с плавно навлизане в изкривяване.",119},
    {"e2","effects","Delay Lagoon","Аналогов делей с tap tempo и до 800 ms.",169},
    {"e3","effects","Fuzz Storm","Гъст винтидж фъз с контрол на гейт.",99},
    {"a1","amps","Bluewire 20","20 W лампов комбо усилвател за вкъщи и репетиции.",549},
    {"a2","amps","Stagebolt 50","50 W комбо с два канала и вграден реверб.",799},
    {"a3","amps","Micro Neon 5","5 W компактен усилвател със слушалков изход.",129},
};
#define NP ((int)(sizeof P / sizeof P[0]))

static const char *CAT_ID[]   = {"guitars", "effects", "amps"};
static const char *CAT_NAME[] = {"Китари", "Ефект педали", "Усилватели"};
static const char *CAT_TEXT[] = {
    "Електрически китари за всеки стил – от блус до метъл.",
    "Овърдрайв, делей, фъз – оформи собствения си звук.",
    "От малки домашни комба до сценични усилватели."};

typedef struct { const char *n, *h; int p; } Opt;
static const Opt COLOR[] = {{"Черно","#111114",0},{"Електрик синьо","#1b3fa0",0},{"Неоново жълто","#f0ff00",40},
                            {"Огнено червено","#c8102e",40},{"Тюркоаз","#18b5a6",40},{"Сребро","#b8bcc4",60}};
static const Opt HEAD[] = {
    {"Gibson – Open Book (3+3)", "", 0},
    {"Fender – 6 в ред", "", 30},
    {"Jackson – Pointed", "", 50}
};
static const Opt BODY[] = {
    {"Gibson – Les Paul", "", 0},
    {"Fender – Stratocaster", "", 80},
    {"Jackson – Dinky", "", 40}
};
static const Opt PICK[]  = {{"SSS (3 сингъла)","",0},{"HH (2 хъмбъкъра)","",90},{"HSS","",60},{"P90 (2 бр.)","",70}};
#define BASE_PRICE 699

/* ---------- кошница (в паметта на сървъра) ---------- */
typedef struct { char id[24]; char name[64]; char desc[160]; int price, qty; } Item;
static Item cart[64];
static int ncart = 0, custom_seq = 1;

static void cart_add(const char *id, const char *name, const char *desc, int price) {
    for (int i = 0; i < ncart; i++)
        if (!strcmp(cart[i].id, id)) { cart[i].qty++; return; }
    if (ncart >= 64) return;
    Item *it = &cart[ncart++];
    snprintf(it->id, sizeof it->id, "%s", id);
    snprintf(it->name, sizeof it->name, "%s", name);
    snprintf(it->desc, sizeof it->desc, "%s", desc);
    it->price = price; it->qty = 1;
}
static int cart_count(void) { int s = 0; for (int i = 0; i < ncart; i++) s += cart[i].qty; return s; }
static void cart_remove(int i) { for (; i < ncart - 1; i++) cart[i] = cart[i + 1]; ncart--; }

/* ---------- разчитане на параметри от адреса ---------- */
static int qget(const char *q, const char *key, char *out, size_t sz) {
    size_t kl = strlen(key);
    for (const char *p = q; *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < sz) out[i++] = *p++;
            out[i] = 0;
            return 1;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return 0;
}
static int qint(const char *q, const char *key, int max, int def) {
    char t[16];
    if (!qget(q, key, t, sizeof t)) return def;
    int v = atoi(t);
    return (v < 0 || v >= max) ? def : v;
}

/* ---------- общи части от страниците ---------- */
static void page_start(Buf *b, const char *title, const char *active, const char *q) {
    static const char *links[][2] = {{"./","home"},{"categories","cat"},{"custom","custom"},{"info","info"},{"profile","profile"}};
    static const char *names[] = {"Начало","Категории","Къстъм","Инфо","Профил"};
    bcat(b, "<!DOCTYPE html><html lang=\"bg\"><head><meta charset=\"UTF-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<title>%s – Sound of the Electricity</title><style>%s</style></head><body>", title, STYLE_CSS);
    bcat(b, "<header class=\"site\"><div class=\"in\"><a class=\"logo\" href=\"./\">Sound of the Electricity</a>"
            "<nav class=\"links\" aria-label=\"Основно меню\">");
    for (int i = 0; i < 5; i++)
        bcat(b, "<a href=\"%s\"%s>%s</a>", links[i][0], !strcmp(links[i][1], active) ? " class=\"active\"" : "", names[i]);
    bcat(b, "</nav><a class=\"cart-link%s\" href=\"cart\">Кошница<span class=\"badge\">%d</span></a></div></header><main>",
         !strcmp(active, "cart") ? " active" : "", cart_count());
    char t[8];
    if (qget(q, "added", t, sizeof t)) bcat(b, "<div class=\"notice\" role=\"status\">Продуктът е добавен в кошницата.</div>");
}
static void page_end(Buf *b) {
    bcat(b, "</main><footer class=\"site\">© 2026 Sound of the Electricity – магазин за китари, ефекти и усилватели</footer></body></html>");
}

static void icon(Buf *b, const char *cat) {
    const char *g = "";
    if (!strcmp(cat, "guitars"))
        g = "<path d=\"M70 12 L82 24 L74 32 L62 54 Q80 62 66 84 Q50 100 34 88 Q22 76 34 62 L40 46 L52 24Z\"/><circle cx=\"52\" cy=\"76\" r=\"4\"/>";
    else if (!strcmp(cat, "effects"))
        g = "<rect x=\"30\" y=\"14\" width=\"60\" height=\"92\" rx=\"8\"/><circle cx=\"60\" cy=\"42\" r=\"13\"/><circle cx=\"60\" cy=\"82\" r=\"7\"/><path d=\"M60 29v8\"/>";
    else
        g = "<rect x=\"14\" y=\"26\" width=\"92\" height=\"70\" rx=\"6\"/><circle cx=\"60\" cy=\"64\" r=\"20\"/><circle cx=\"60\" cy=\"64\" r=\"6\"/><path d=\"M26 38h20M74 38h20\"/>";
    bcat(b, "<svg width=\"120\" height=\"120\" viewBox=\"0 0 120 120\" aria-hidden=\"true\" fill=\"none\" stroke=\"#f0ff00\" "
            "stroke-width=\"3\" stroke-linecap=\"round\" stroke-linejoin=\"round\">%s</svg>", g);
}

static void card(Buf *b, const Product *p, const char *back) {
    bcat(b, "<article class=\"card\"><div class=\"pic\">");
    icon(b, p->cat);
    bcat(b, "</div><div class=\"body\"><h3>%s</h3><p>%s</p><div class=\"row\"><span class=\"price\">%d €</span>"
            "<a class=\"btn small\" href=\"add?id=%s&amp;back=%s\">В кошницата</a></div></div></article>",
         p->name, p->desc, p->price, p->id, back);
}

/* до 3 продукта от категория */
static void grid(Buf *b, int cat, const char *back) {
    bcat(b, "<div class=\"grid\">");
    int shown = 0;
    for (int i = 0; i < NP && shown < 3; i++)
        if (!strcmp(P[i].cat, CAT_ID[cat])) { card(b, &P[i], back); shown++; }
    bcat(b, "</div>");
}

/* ---------- страници ---------- */
static void page_home(Buf *b, const char *q) {
    page_start(b, "Начало", "home", q);
    bcat(b, "<section class=\"hero\"><h1>Sound of the Electricity</h1>"
            "<p>Китари, ефект педали и усилватели за всеки, който иска звукът му да се чува. Или създай своя китара от нулата.</p>"
            "<a class=\"btn\" href=\"custom\">Направи своя къстъм</a>"
            "<a class=\"btn ghost\" href=\"categories\">Всички категории</a></section>");
    for (int c = 0; c < 3; c++) {
        bcat(b, "<section class=\"cat-section\"><div class=\"head\"><h2>%s</h2>"
                "<a class=\"btn small ghost\" href=\"categories#%s\">Виж категорията</a></div>", CAT_NAME[c], CAT_ID[c]);
        grid(b, c, "home");
        bcat(b, "</section>");
    }
    bcat(b, "<div class=\"cta-band\"><div><h2>Не намираш точно това, което търсиш?</h2>"
            "<p class=\"lead\" style=\"margin:0\">Избери цвят, хедсток, форма на тялото и пикъпи – и виж китарата си на живо.</p></div>"
            "<a class=\"btn\" href=\"custom\">Към къстъм конфигуратора</a></div>");
    page_end(b);
}

static void page_categories(Buf *b, const char *q) {
    page_start(b, "Категории", "cat", q);
    bcat(b, "<h2>Категории</h2><p class=\"lead\">Избери какво търсиш – във всяка категория има най-добрите ни предложения.</p>"
            "<nav class=\"subnav\" aria-label=\"Категории\">");
    for (int c = 0; c < 3; c++) bcat(b, "<a class=\"btn small\" href=\"#%s\">%s</a>", CAT_ID[c], CAT_NAME[c]);
    bcat(b, "</nav>");
    for (int c = 0; c < 3; c++) {
        bcat(b, "<section class=\"cat-section\" id=\"%s\"><div class=\"head\"><h2>%s</h2></div><p class=\"lead\">%s</p>",
             CAT_ID[c], CAT_NAME[c], CAT_TEXT[c]);
        grid(b, c, "categories");
        bcat(b, "</section>");
    }
    page_end(b);
}

/* Повтарящите се SVG елементи се чертаят с малки шаблонни функции. */
static void svg_circle(Buf *b, int x, int y, double r, const char *fill) {
    bcat(b, "<circle cx=\"%d\" cy=\"%d\" r=\"%.1f\" fill=\"%s\"/>", x, y, r, fill);
}

static void svg_pickup(Buf *b, int y, int w, int h, int p90) {
    int x = 80 - w / 2;
    bcat(b, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"%d\" "
            "fill=\"#202126\" stroke=\"#aaa\" stroke-width=\"1.3\"/>",
         x, y, w, h, p90 ? 6 : 2);
    for (int i = 0; i < 6; ++i)
        svg_circle(b, x + 5 + i * (w - 10) / 5, y + h / 2, 1.25, "#c7c7c7");
}

static void guitar_svg(Buf *b, const int s[4]) {
    /* Контурите са отделни шаблони; останалите детайли се споделят. */
    static const char *head[] = {
        "M70 53 L65 43 L58 37 L52 28 Q48 22 52 16 L59 8 Q63 4 68 8 L80 15 L92 8 Q97 4 101 9 L108 17 Q112 23 107 29 L96 40 L92 53Z",
        "M70 53 L68 17 Q67 9 73 5 Q78 1 86 3 L96 6 Q101 8 100 14 L96 53Z",
        "M70 53 L57 12 L70 17 L78 2 L87 16 L105 8 L96 53Z"
    };
    static const char *body[] = {
        /* Les Paul: пълно горно рамо и единична изрезка. */
        "M64 220 C48 211 35 218 34 236 C33 249 45 258 49 266 C39 283 31 304 34 326 C38 353 58 369 81 369 C107 369 127 348 130 321 C133 296 120 273 108 255 C103 247 106 238 117 231 C125 225 120 214 110 211 C97 207 89 219 86 232 L69 232 C68 226 67 222 64 220Z",
        /* Stratocaster: две изрезки и плавна талия. */
        "M57 215 C47 207 35 209 31 220 C27 231 37 241 49 249 C39 259 32 272 32 291 C31 321 43 350 63 363 C73 369 87 369 97 363 C117 350 129 321 128 291 C128 272 121 259 111 249 C123 241 133 231 129 220 C125 209 113 207 103 215 L91 231 L69 231Z",
        /* Jackson Dinky: Superstrat с остри рога и отчетлива талия. */
        "M54 211 L69 229 L80 218 L92 229 L108 211 L105 241 L128 253 L114 279 L132 326 L104 352 L81 342 L57 352 L29 326 L46 279 L32 253 L56 241Z"
    };
    static const int tuner[3][6][2] = {
        {{53,20},{55,30},{62,39},{104,20},{102,30},{95,39}},
        {{99,9},{99,17},{98,25},{97,33},{96,41},{95,49}},
        {{58,15},{62,27},{66,39},{104,13},{101,26},{98,39}}
    };
    /* y, ширина, височина: три конфигурации пикъпи. */
    static const int pickups[4][3][3] = {
        {{245,44,12},{268,44,12},{291,44,12}},
        {{252,46,18},{290,46,18},{0,0,0}},
        {{250,46,18},{276,44,12},{296,44,12}},
        {{252,46,18},{290,46,18},{0,0,0}}
    };
    const int h = s[1], model = s[2], p = s[3];
    const char *line = "stroke=\"#252525\" stroke-width=\"2\" stroke-linejoin=\"round\"";

    bcat(b, "<svg viewBox=\"0 0 160 380\" role=\"img\" aria-label=\"Персонализирана китара\">");
    bcat(b, "<path d=\"%s\" fill=\"#9a6638\" %s/>", head[h], line);
    for (int i = 0; i < 6; ++i) {
        int x = tuner[h][i][0], y = tuner[h][i][1];
        bcat(b, "<circle cx=\"%d\" cy=\"%d\" r=\"4\" fill=\"#ddd\" stroke=\"#555\"/>", x, y);
    }
    bcat(b, "<rect x=\"72\" y=\"48\" width=\"16\" height=\"177\" rx=\"2\" fill=\"#5b3922\" stroke=\"#2b1b12\"/>");
    for (int i = 0; i < 12; ++i)
        bcat(b, "<path d=\"M72 %dH88\" stroke=\"#c6b8a5\" stroke-width=\".8\"/>", 65 + i * 11);
    bcat(b, "<path d=\"%s\" fill=\"%s\" %s/>", body[model], COLOR[s[0]].h, line);

    /* Pickguard: специфична форма за всеки корпус. */
    if (model == 1)
        bcat(b, "<path d=\"M54 239Q61 226 80 230Q101 226 108 245L103 292Q80 310 57 292Z\" fill=\"#d8d5c9\" stroke=\"#777\"/>");
    else if (model == 0)
        bcat(b, "<path d=\"M55 239Q80 229 105 239L104 302Q80 311 56 302Z\" fill=\"#d8d5c9\" fill-opacity=\".16\" stroke=\"#aaa\"/>");
    else
        bcat(b, "<path d=\"M55 239L80 229L105 239L101 302L80 311L59 302Z\" fill=\"#111318\" fill-opacity=\".35\" stroke=\"#777\"/>");

    for (int i = 0; i < 3; ++i)
        if (pickups[p][i][1])
            svg_pickup(b, pickups[p][i][0], pickups[p][i][1], pickups[p][i][2], p == 3);

    bcat(b, "<rect x=\"59\" y=\"326\" width=\"42\" height=\"8\" rx=\"2\" fill=\"#c4c5c8\" stroke=\"#555\"/>");
    for (int i = 0; i < 6; ++i) {
        int x = 75 + i * 2;
        bcat(b, "<path d=\"M%d 48L%d 331\" stroke=\"#eee\" stroke-width=\".7\"/>", x, 72 + i * 3);
    }
    if (model == 0) {
        svg_circle(b,108,310,5,"#d7d7d7"); svg_circle(b,116,322,5,"#d7d7d7");
    } else if (model == 1) {
        svg_circle(b,104,294,4.5,"#eee"); svg_circle(b,112,306,4.5,"#eee"); svg_circle(b,108,319,4.5,"#eee");
    } else svg_circle(b,108,313,4.5,"#c8c8c8");
    bcat(b, "</svg>");
}

static int custom_price(const int s[4]) { return BASE_PRICE + COLOR[s[0]].p + HEAD[s[1]].p + BODY[s[2]].p + PICK[s[3]].p; }

static void opt_group(Buf *b, const char *title, const Opt *o, int n, int which, const int s[4]) {
    bcat(b, "<section class=\"opt\"><h3>%s</h3><div class=\"choices\">", title);
    for (int i = 0; i < n; i++) {
        int t[4] = {s[0], s[1], s[2], s[3]};
        t[which] = i;
        bcat(b, "<a class=\"choice%s\" href=\"custom?c=%d&amp;h=%d&amp;b=%d&amp;p=%d\"%s>",
             i == s[which] ? " on" : "", t[0], t[1], t[2], t[3], i == s[which] ? " aria-current=\"true\"" : "");
        if (which == 0) bcat(b, "<span class=\"swatch\" style=\"background:%s\"></span>", o[i].h);
        bcat(b, "%s", o[i].n);
        if (o[i].p) bcat(b, " (+%d €)", o[i].p);
        bcat(b, "</a>");
    }
    bcat(b, "</div></section>");
}

static void sel_from_query(const char *q, int s[4]) {
    s[0] = qint(q, "c", 6, 1);
    s[1] = qint(q, "h", 3, 0);
    s[2] = qint(q, "b", 3, 0);
    s[3] = qint(q, "p", 4, 0);
}

static void page_custom(Buf *b, const char *q) {
    int s[4];
    sel_from_query(q, s);
    page_start(b, "Къстъм", "custom", q);
    bcat(b, "<h2>Къстъм китара</h2><p class=\"lead\">Конфигурирай своята китара. Натисни опция и китарата се обновява, заедно с цената.</p>"
            "<div class=\"cfg\"><div class=\"preview\">");
    guitar_svg(b, s);
    bcat(b, "</div><div>");
    opt_group(b, "Цвят", COLOR, 6, 0, s);
    opt_group(b, "Форма на хедстока", HEAD, 3, 1, s);
    opt_group(b, "Форма на тялото", BODY, 3, 2, s);
    opt_group(b, "Пикъпи", PICK, 4, 3, s);
    bcat(b, "<div class=\"total\"><span>Крайна цена: <span class=\"price\">%d €</span></span>"
            "<a class=\"btn\" href=\"addc?c=%d&amp;h=%d&amp;b=%d&amp;p=%d\">Добави в кошницата</a></div></div></div>",
         custom_price(s), s[0], s[1], s[2], s[3]);
    page_end(b);
}

static void page_info(Buf *b, const char *q) {
    page_start(b, "Инфо", "info", q);
    bcat(b, "<h2>За нас</h2><p class=\"lead\">Sound of the Electricity е магазин за хора, които живеят с китарата. "
            "Подбираме инструменти, които звучат добре и издържат на сцена и в репетиционна.</p><div class=\"info-grid\">"
            "<div class=\"box\"><h3>Адрес</h3><p>ул. Примерна 1, Пловдив<br>(примерен адрес)</p></div>"
            "<div class=\"box\"><h3>Работно време</h3><p>Пон – Пет: 10:00 – 19:00<br>Събота: 10:00 – 15:00<br>Неделя: почивен ден</p></div>"
            "<div class=\"box\"><h3>Контакти</h3><p>Тел.: 0888 000 000<br>Имейл: info@soundoftheelectricity.example</p></div>"
            "<div class=\"box\"><h3>Доставка и гаранция</h3><p>Доставка до 3 работни дни. 2 години гаранция за всички инструменти и усилватели.</p></div>"
            "<div class=\"box\"><h3>Къстъм поръчки</h3><p>Изработката на къстъм китара отнема около 4 седмици. Можеш да я вземеш от магазина или да я получиш с куриер.</p></div>"
            "</div>");
    page_end(b);
}

static void page_profile(Buf *b, const char *q) {
    char t[8];
    page_start(b, "Профил", "profile", q);
    bcat(b, "<h2>Профил</h2><p class=\"lead\">Демо страница – данните не се запазват.</p>");
    if (qget(q, "saved", t, sizeof t)) bcat(b, "<div class=\"notice\" role=\"status\">Демо профил – данните не се запазват.</div>");
    bcat(b, "<div class=\"profile\"><div class=\"avatar\" aria-hidden=\"true\">SE</div>"
            "<form class=\"form\" action=\"profile\" method=\"get\"><input type=\"hidden\" name=\"saved\" value=\"1\">"
            "<label>Име<input type=\"text\" value=\"Иван Петров\"></label>"
            "<label>Имейл<input type=\"email\" value=\"ivan@example.com\"></label>"
            "<label>Телефон<input type=\"tel\" value=\"0888 123 456\"></label>"
            "<label>Любим стил<select><option>Рок</option><option>Блус</option><option>Метъл</option><option>Джаз</option></select></label>"
            "<button class=\"btn\" type=\"submit\">Запази</button></form></div>");
    page_end(b);
}

static void page_cart(Buf *b, const char *q) {
    char t[8];
    page_start(b, "Кошница", "cart", q);
    bcat(b, "<h2>Кошница</h2>");
    if (qget(q, "ordered", t, sizeof t)) bcat(b, "<div class=\"notice\" role=\"status\">Демо магазин – поръчката не се изпраща.</div>");
    if (ncart == 0) {
        bcat(b, "<p class=\"lead\">Кошницата е празна. Разгледай <a href=\"categories\">категориите</a> или направи своя <a href=\"custom\">къстъм китара</a>.</p>");
        page_end(b);
        return;
    }
    int total = 0;
    bcat(b, "<div class=\"cart-wrap\"><table class=\"cart\"><thead><tr><th>Продукт</th><th>Цена</th><th>Брой</th><th>Сума</th><th></th></tr></thead><tbody>");
    for (int i = 0; i < ncart; i++) {
        total += cart[i].price * cart[i].qty;
        bcat(b, "<tr><td>%s", cart[i].name);
        if (cart[i].desc[0]) bcat(b, "<br><small>%s</small>", cart[i].desc);
        bcat(b, "</td><td>%d €</td><td><span class=\"qty\"><a href=\"qty?id=%s&amp;d=-1\" aria-label=\"Намали\">−</a>%d"
                "<a href=\"qty?id=%s&amp;d=1\" aria-label=\"Увеличи\">+</a></span></td><td>%d €</td>"
                "<td><a class=\"btn small ghost\" href=\"rm?id=%s\">Премахни</a></td></tr>",
             cart[i].price, cart[i].id, cart[i].qty, cart[i].id, cart[i].price * cart[i].qty, cart[i].id);
    }
    bcat(b, "</tbody></table></div><div class=\"total\"><span class=\"price\">Общо: %d €</span>"
            "<a class=\"btn\" href=\"cart?ordered=1\">Поръчай</a></div>", total);
    page_end(b);
}

/* ---------- HTTP ---------- */
static void send_all(int fd, const char *d, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, d, n);
        if (w <= 0) return;
        d += w; n -= (size_t)w;
    }
}
static void send_redirect(int fd, const char *where) {
    char h[512];
    int n = snprintf(h, sizeof h, "HTTP/1.1 303 See Other\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", where);
    send_all(fd, h, (size_t)n);
}
static void send_page(int fd, int code, const char *status, Buf *b) {
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     code, status, b->n);
    send_all(fd, h, (size_t)n);
    send_all(fd, b->d, b->n);
}

/* безопасен адрес за връщане след добавяне */
static const char *safe_back(const char *x) {
    if (!strcmp(x, "categories")) return "categories";
    return "./";
}

/* взима последната част от адреса и маха ".html": "/x/custom.html" -> "custom" */
static void route_name(const char *path, char *out, size_t sz) {
    const char *last = strrchr(path, '/');
    last = last ? last + 1 : path;
    snprintf(out, sz, "%s", last);
    size_t n = strlen(out);
    if (n > 5 && !strcmp(out + n - 5, ".html")) out[n - 5] = 0;
    if (!strcmp(out, "index")) out[0] = 0;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void handle(int fd, char *rawpath, char *query) {
    char id[32], t[16], path[64];
    Buf b = {0};
    route_name(rawpath, path, sizeof path);

    if (!strcmp(path, "add")) {
        if (qget(query, "id", id, sizeof id)) {
            for (int i = 0; i < NP; i++)
                if (!strcmp(P[i].id, id)) cart_add(P[i].id, P[i].name, "", P[i].price);
        }
        char back[32] = "home", loc[64];
        qget(query, "back", back, sizeof back);
        snprintf(loc, sizeof loc, "%s?added=1", safe_back(back));
        send_redirect(fd, loc);
        return;
    }
    if (!strcmp(path, "addc")) {
        int s[4];
        sel_from_query(query, s);
        char key[24], desc[160];
        snprintf(key, sizeof key, "c-%d", custom_seq++);
        snprintf(desc, sizeof desc, "%s, хедсток %s, %s, пикъпи %s", COLOR[s[0]].n, HEAD[s[1]].n, BODY[s[2]].n, PICK[s[3]].n);
        cart_add(key, "Къстъм китара", desc, custom_price(s));
        send_redirect(fd, "cart");
        return;
    }
    if (!strcmp(path, "qty") || !strcmp(path, "rm")) {
        if (qget(query, "id", id, sizeof id)) {
            for (int i = 0; i < ncart; i++) {
                if (strcmp(cart[i].id, id)) continue;
                if (!strcmp(path, "rm")) cart_remove(i);
                else {
                    int d = qget(query, "d", t, sizeof t) && atoi(t) < 0 ? -1 : 1;
                    cart[i].qty += d;
                    if (cart[i].qty <= 0) cart_remove(i);
                }
                break;
            }
        }
        send_redirect(fd, "cart");
        return;
    }

    int code = 200;
    const char *status = "OK";
    if      (!strcmp(path, ""))           page_home(&b, query);
    else if (!strcmp(path, "categories")) page_categories(&b, query);
    else if (!strcmp(path, "custom"))     page_custom(&b, query);
    else if (!strcmp(path, "info"))       page_info(&b, query);
    else if (!strcmp(path, "profile"))    page_profile(&b, query);
    else if (!strcmp(path, "cart"))       page_cart(&b, query);
    else {
        code = 404; status = "Not Found";
        page_start(&b, "Не е намерена", "", "");
        bcat(&b, "<h2>Страницата не е намерена</h2><p class=\"lead\">Върни се към <a href=\"./\">началото</a>.</p>");
        page_end(&b);
    }
    send_page(fd, code, status, &b);
    free(b.d);
}

/* всяка връзка се обслужва в отделна нишка, за да не блокира сайтът */
static void *worker(void *arg) {
    int c = (int)(long)arg;
    struct timeval tv = {2, 0};   /* браузърите понякога отварят празни връзки */
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char req[4096], method[8], target[1024];
    ssize_t n = read(c, req, sizeof req - 1);
    if (n > 0) {
        req[n] = 0;
        if (sscanf(req, "%7s %1023s", method, target) == 2 && !strcmp(method, "GET")) {
            char *q = strchr(target, '?');
            if (q) *q++ = 0; else q = target + strlen(target);
            char *h = strchr(q, '#');
            if (h) *h = 0;
            pthread_mutex_lock(&lock);
            handle(c, target, q);
            pthread_mutex_unlock(&lock);
        }
    }
    close(c);
    return NULL;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(srv, 32) < 0) { perror("listen"); return 1; }
    printf("Sound of the Electricity работи на порт %d (http://localhost:%d)\n", port, port);
    fflush(stdout);

    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        pthread_t th;
        if (pthread_create(&th, NULL, worker, (void *)(long)c) == 0) pthread_detach(th);
        else close(c);
    }
}