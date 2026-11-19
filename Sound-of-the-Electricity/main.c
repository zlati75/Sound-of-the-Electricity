/* ===== Sound of the Electricity – малък уеб сървър за магазина ===== */

#define _GNU_SOURCE

/* Windows и Linux имат различни мрежови библиотеки – тук избираме правилната */
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET sock_t;          /* тип на сокет */
#define CLOSE closesocket       /* как се затваря сокет */
#else
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSE close
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>

#include "data.h"             /* цените и опциите за къстъм китарата */

#define N(a) ((int)(sizeof(a) / sizeof *(a)))   /* брой елементи в масив */
#define SITE "site"                             /* папката със сайта */
#define HTML "text/html; charset=utf-8"

/* Заявка: коя страница се иска и какво има след "?" в адреса */
typedef struct { const char *page, *query; } Req;
/* Продукт от каталога (чете се от HTML файловете) */
typedef struct { char id[24], name[64]; int price; } Product;
/* Ред в кошницата */
typedef struct { char id[24], name[64], desc[400]; int price, qty; } Item;

/* Динамичен текст (буфер), в който събираме готовата страница */
typedef struct { char *d; size_t n, cap; } Buf;

/* Добавя n байта към буфера и го уголемява, ако не стига място */
static void add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        char *p = realloc(b->d, (b->n + n + 1) * 2);
        if (!p) return;                 /* няма памет – пропускаме */
        b->d = p;
        b->cap = (b->n + n + 1) * 2;
    }
    memcpy(b->d + b->n, s, n);
    b->n += n;
}

/* Добавя цял низ към буфера */
static void str(Buf *b, const char *s) {
    add(b, s, strlen(s));
}

/* Като printf, но пише в буфера (до 2047 знака наведнъж) */
static void put(Buf *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    str(b, tmp);
}

static Product catalog[128];    /* всички продукти от магазина */
static Item cart[64];           /* кошницата (една за всички посетители) */
static int nprod, ncart, custom_seq = 1;    /* брой продукти, брой редове в кошницата, номер на къстъм китара */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;    /* заключва данните, докато ги ползва една нишка */

/* Чете целия файл в паметта. Връща NULL при грешка; len е дължината му. Трябва да се извика free() */
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);              /* отиваме в края, за да разберем размера */
    n = ftell(f);
    rewind(f);
    if (n >= 0 && (buf = malloc((size_t)n + 1))) {
        *len = fread(buf, 1, (size_t)n, f);
        buf[*len] = 0;                  /* край на низа */
    }
    fclose(f);
    return buf;
}

/* Търси в текста между from и to част, която започва с open и свършва с close.
   Копира я в out. Пример: между "<h3>" и "</h3>" се намира името на продукта. */
static int between(const char *from, const char *to, const char *open, const char *close, char *out, size_t size) {
    const char *a = strstr(from, open), *b;
    if (!a || a >= to) return 0;
    a += strlen(open);
    b = strstr(a, close);
    if (!b || b > to) return 0;
    snprintf(out, size, "%.*s", (int)(b - a), a);
    return 1;
}

/* Взима стойността на параметър от адреса. Пример: в "id=g1&x=2" ключът "x" дава "2" */
static int qget(const char *q, const char *key, char *out, size_t size) {
    size_t kl = strlen(key);
    while (*q) {
        size_t len = strcspn(q, "&");   /* дължина на един параметър (до "&") */
        if (len > kl && !strncmp(q, key, kl) && q[kl] == '=') {
            snprintf(out, size, "%.*s", (int)(len - kl - 1), q + kl + 1);
            return 1;
        }
        q += len + (q[len] == '&');     /* прескачаме към следващия параметър */
    }
    return 0;
}

/* Проверява дали параметърът name има точно стойност value */
static int qhas(const char *q, const char *name, const char *value) {
    char v[48];
    return qget(q, name, v, sizeof v) && !strcmp(v, value);
}

/* Търси в HTML текста всички <article> блокове и записва продуктите в каталога.
   От всеки блок се четат: id (в add?id=...), името (в <h3>) и цената (class="price") */
static void scan_products(const char *html) {
    const char *a = html, *end;
    while (nprod < N(catalog) && (a = strstr(a, "<article")) && (end = strstr(a, "</article>"))) {
        Product *p = &catalog[nprod];
        char price[16];
        if (between(a, end, "add?id=", "\"", p->id, sizeof p->id) &&
            between(a, end, "<h3>", "</h3>", p->name, sizeof p->name) &&
            between(a, end, "class=\"price\">", "<", price, sizeof price)) {
            p->price = atoi(price);     /* "849 €" -> 849 */
            nprod++;
        }
        a = end;                        /* продължаваме със следващия продукт */
    }
}

/* Зарежда каталога наново от всички файлове в site/parts (китари, педали, усилватели) */
static void load_catalog(void) {
    DIR *d = opendir(SITE "/parts");
    struct dirent *e;
    nprod = 0;
    while (d && (e = readdir(d))) {
        char path[300];
        size_t len;
        char *html;
        if (!strstr(e->d_name, ".html")) continue;     /* само .html файловете */
        snprintf(path, sizeof path, SITE "/parts/%s", e->d_name);
        if ((html = read_file(path, &len))) scan_products(html);
        free(html);
    }
    if (d) closedir(d);
}

/* Намира продукт по id. Връща NULL, ако няма такъв */
static const Product *find_product(const char *id) {
    for (int i = 0; i < nprod; i++)
        if (!strcmp(catalog[i].id, id)) return &catalog[i];
    return NULL;
}

/* Добавя продукт в кошницата. Ако вече го има, увеличава броя му */
static void cart_add(const char *id, const char *name, const char *desc, int price) {
    for (int i = 0; i < ncart; i++)
        if (!strcmp(cart[i].id, id)) { cart[i].qty++; return; }
    if (ncart >= N(cart)) return;       /* кошницата е пълна */
    Item *it = &cart[ncart++];
    snprintf(it->id, sizeof it->id, "%s", id);
    snprintf(it->name, sizeof it->name, "%s", name);
    snprintf(it->desc, sizeof it->desc, "%s", desc);
    it->price = price;
    it->qty = 1;
}

/* Общ брой продукти в кошницата (показва се в менюто) */
static int cart_count(void) {
    int sum = 0;
    for (int i = 0; i < ncart; i++) sum += cart[i].qty;
    return sum;
}

/* Връща етикета на поле (напр. "body" -> "Модел"). NULL, ако полето е допълнително (калъф, каиш...) */
static const char *field_label(const char *name) {
    for (int i = 0; i < N(FIELD); i++)
        if (!strcmp(FIELD[i].name, name)) return FIELD[i].label;
    return NULL;
}

/* Намира избраната опция за дадено поле. За модел и хедсток се гледат само опциите на избраната марка
   (напр. при марка "fender" стойността трябва да започва с "fender-") */
static const Choice *picked(const char *q, const char *field, const char *brand) {
    size_t bl = strlen(brand);
    int needs_brand = !strcmp(field, "body") || !strcmp(field, "head");
    for (int i = 0; i < N(CHOICES); i++) {
        const Choice *c = &CHOICES[i];
        if (strcmp(c->name, field) || !qhas(q, field, c->value)) continue;
        if (needs_brand && (strncmp(c->value, brand, bl) || c->value[bl] != '-')) continue;
        return c;
    }
    return NULL;
}

/* Събира всички избрани опции в sel и връща колко са.
   В missing записва имената на задължителните полета, които не са избрани */
static int choose(const char *q, const Choice **sel, char *missing, size_t size) {
    char brand[16] = "";
    int n = 0;
    qget(q, "brand", brand, sizeof brand);
    missing[0] = 0;
    for (int i = 0; i < N(FIELD); i++) {            /* задължителните полета */
        const Choice *c = picked(q, FIELD[i].name, brand);
        if (c) sel[n++] = c;
        else snprintf(missing + strlen(missing), size - strlen(missing), "%s%s", *missing ? ", " : "", FIELD[i].label);
    }
    for (int i = 0; i < N(CHOICES); i++)            /* допълнителните (калъф, каиш...) */
        if (!field_label(CHOICES[i].name) && qhas(q, CHOICES[i].name, CHOICES[i].value)) sel[n++] = &CHOICES[i];
    return n;
}

/* Сумата от цените на избраните опции */
static int total_price(const Choice **sel, int n) {
    int total = 0;
    for (int i = 0; i < n; i++) total += sel[i]->price;
    return total;
}

/* Рисува таблицата с цената на къстъм китарата (само ако е натиснато "Изчисли") */
static void summary(Buf *o, const char *q) {
    const Choice *sel[N(CHOICES)];
    char flag[4], missing[160];
    int n;
    if (!qget(q, "calc", flag, sizeof flag)) return;
    n = choose(q, sel, missing, sizeof missing);
    str(o, "<div class=\"summary\">");
    if (*missing) put(o, "<p class=\"notice\">Избери: %s.</p>", missing);    /* нещо липсва */
    else {
        str(o, "<table>");
        for (int i = 0; i < n; i++) {
            const char *fl = field_label(sel[i]->name);
            put(o, "<tr><td>%s%s%s</td><td>%d €</td></tr>", fl ? fl : "", fl ? ": " : "", sel[i]->label, sel[i]->price);
        }
        put(o, "<tr class=\"total\"><td>Общо</td><td>%d €</td></tr></table>", total_price(sel, n));
    }
    str(o, "</div>");
}

/* Предварително обявяване – render и include се викат взаимно */
static void render(Buf *o, const char *t, const Req *r, int depth);

/* Вмъква файл от site/parts (маркер {{include:име}}). depth пази от безкрайно вмъкване,
   а проверката за "./\" не позволява достъп до други папки */
static void include(Buf *o, const char *name, const Req *r, int depth) {
    char path[160];
    size_t len;
    char *html;
    if (depth > 4 || strpbrk(name, "./\\")) return;
    snprintf(path, sizeof path, SITE "/parts/%s.html", name);
    if ((html = read_file(path, &len))) render(o, html, r, depth + 1);
    free(html);
}

/* Рисува кошницата като таблица (маркер {{cart}}) */
static void cart_html(Buf *o, const Req *r, int depth) {
    int total = 0;
    if (!ncart) {
        str(o, "<p>Кошницата е празна. Разгледай <a href=\"./\">магазина</a> или направи своя <a href=\"custom\">къстъм китара</a>.</p>");
        return;
    }
    str(o, "<table><tr><th>Продукт</th><th>Цена</th><th>Брой</th><th>Сума</th><th></th></tr>");
    for (int i = 0; i < ncart; i++) {
        const Item *it = &cart[i];
        total += it->price * it->qty;
        put(o, "<tr><td>%s", it->name);
        if (*it->desc) put(o, "<br><small class=\"muted\">%s</small>", it->desc);   /* описание на къстъм китарата */
        put(o, "</td><td>%d €</td><td>%d</td><td>%d €</td>"
               "<td><a href=\"rm?id=%s\">Премахни</a></td></tr>",
            it->price, it->qty, it->price * it->qty, it->id);
    }
    put(o, "<tr class=\"total\"><td colspan=\"3\">Общо</td><td colspan=\"2\">%d €</td></tr></table>", total);
    include(o, "checkout", r, depth);       /* формата за поръчка под таблицата */
}

/* Обработва един маркер {{...}} от страницата. m е текстът между скобите */
static void marker(Buf *o, const char *m, size_t len, const Req *r, int depth) {
    char name[48], *arg;
    snprintf(name, sizeof name, "%.*s", (int)len, m);
    arg = strchr(name, ':');                /* "include:header" се разделя на "include" и "header" */
    if (arg) *arg++ = 0;
    if (!strcmp(name, "include") && arg) include(o, arg, r, depth);
    else if (!strcmp(name, "active") && arg && !strcmp(arg, r->page)) str(o, "class=\"active\"");  /* подчертава текущата страница в менюто */
    else if (!strcmp(name, "cart_count")) put(o, "%d", cart_count());
    else if (!strcmp(name, "cart")) cart_html(o, r, depth);
    else if (!strcmp(name, "summary")) summary(o, r->query);
}

/* Копира таг от формата (<input>, <option> или <select>) и добавя "checked" или "selected",
   ако стойността му е избрана в адреса. Така формата помни избора след "Изчисли".
   select пази името на последния <select>, за да знаем към кое поле принадлежи <option> */
static const char *form_tag(Buf *o, const char *t, const char *q, char *select) {
    const char *end = strchr(t, '>');
    int is_input = !strncmp(t, "<input", 6), is_option = !strncmp(t, "<option", 7);
    char name[32] = "", value[48];
    if (!end) { str(o, t); return t + strlen(t); }
    if (is_input) between(t, end, " name=\"", "\"", name, sizeof name);
    else if (is_option) snprintf(name, sizeof name, "%s", select);
    else between(t, end, " name=\"", "\"", select, 32);     /* <select> – запомняме името му */
    add(o, t, (size_t)(end - t));           /* самият таг без ">" */
    if ((is_input || is_option) && between(t, end, " value=\"", "\"", value, sizeof value) && qhas(q, name, value))
        str(o, is_option ? " selected" : " checked");
    add(o, ">", 1);
    return end + 1;
}

/* Обработва HTML шаблон: копира го и заменя маркерите {{...}} и формулярните тагове */
static void render(Buf *o, const char *t, const Req *r, int depth) {
    char select[32] = "";
    while (*t) {
        const char *end;
        size_t n = strcspn(t, "{<");        /* обикновен текст до следващия "{" или "<" */
        add(o, t, n);
        t += n;
        if (!strncmp(t, "{{", 2) && (end = strstr(t, "}}"))) {
            marker(o, t + 2, (size_t)(end - t - 2), r, depth);
            t = end + 2;
        } else if (!strncmp(t, "<input", 6) || !strncmp(t, "<option", 7) || !strncmp(t, "<select", 7))
            t = form_tag(o, t, r->query, select);
        else if (*t)
            add(o, t++, 1);                 /* друг знак – копираме го */
    }
}

/* От адреса на предишната страница (Referer) взима името ѝ, за да се върнем там след "добави" */
static void back_page(const char *referer, char *out, size_t size) {
    const char *p = referer ? strstr(referer, "://") : NULL;
    char *cut;
    p = p ? strchr(p + 3, '/') : NULL;      /* пропускаме "http://localhost:8080" */
    snprintf(out, size, "%s", p ? p + 1 : "");
    out[strcspn(out, "?#")] = 0;            /* махаме всичко след "?" */
    if ((cut = strstr(out, ".html"))) *cut = 0;
    if (!*out || strpbrk(out, ":\\") || out[0] == '/') snprintf(out, size, "./");   /* при съмнение – началото */
}

/* Изпълнява действие (add, addc, rm, order). Връща адреса за пренасочване или NULL, ако това не е действие */
static const char *run_action(const char *path, const char *q, const char *referer, char *loc, size_t size) {
    char id[32];
    if (!strcmp(path, "add")) {                 /* добавяне на продукт от магазина */
        const Product *p;
        char back[64];
        load_catalog();
        if (qget(q, "id", id, sizeof id) && (p = find_product(id))) cart_add(p->id, p->name, "", p->price);
        back_page(referer, back, sizeof back);
        snprintf(loc, size, "%s", back);        /* връщаме се на страницата, от която е натиснато */
    } else if (!strcmp(path, "addc")) {         /* добавяне на къстъм китара */
        const Choice *sel[N(CHOICES)];
        char missing[160], key[24], desc[400] = "";
        int n = choose(q, sel, missing, sizeof missing);
        if (*missing) {                         /* нещо не е избрано – връщаме към формата */
            snprintf(loc, size, "custom?%s&calc=1", q);
            return loc;
        }
        for (int i = 0; i < n; i++) {           /* правим описание: "Марка: Fender, Модел: ..." */
            const char *fl = field_label(sel[i]->name);
            snprintf(desc + strlen(desc), sizeof desc - strlen(desc), "%s%s%s%s",
                     i ? ", " : "", fl ? fl : "", fl ? ": " : "", sel[i]->label);
        }
        snprintf(key, sizeof key, "c-%d", custom_seq++);   /* всяка къстъм китара е отделен ред */
        cart_add(key, "Къстъм китара", desc, total_price(sel, n));
        return "cart";
    } else if (!strcmp(path, "rm")) {           /* премахване на ред от кошницата */
        if (qget(q, "id", id, sizeof id))
            for (int i = 0; i < ncart; i++)
                if (!strcmp(cart[i].id, id)) {
                    memmove(&cart[i], &cart[i + 1], (size_t)(--ncart - i) * sizeof *cart);   /* изместваме следващите назад */
                    break;
                }
        return "cart";
    } else if (!strcmp(path, "order")) {        /* поръчка – само изчистваме кошницата (демо) */
        ncart = 0;
        return "cart";
    } else
        return NULL;                            /* не е действие – ще се търси файл */
    return loc;
}

/* Връща типа на файла по разширението му – браузърът така знае как да го покаже */
static const char *mime(const char *file) {
    static const char *T[][2] = {{".html", HTML}, {".css", "text/css; charset=utf-8"}, {".svg", "image/svg+xml"},
                                 {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".png", "image/png"},
                                 {".webp", "image/webp"}, {".ico", "image/x-icon"}};
    size_t n = strlen(file);
    for (int i = 0; i < N(T); i++) {
        size_t e = strlen(T[i][0]);
        if (n > e && !strcmp(file + n - e, T[i][0])) return T[i][1];   /* файлът свършва с това разширение */
    }
    return "application/octet-stream";      /* непознат тип */
}

/* Изпраща всичките n байта (send може да изпрати само част от тях, затова е в цикъл) */
static void send_all(sock_t sock, const char *d, size_t n) {
    while (n) {
        int w = (int)send(sock, d, (int)n, 0);
        if (w <= 0) return;                 /* връзката е прекъсната */
        d += w;
        n -= (size_t)w;
    }
}

/* Обработва една заявка: или изпълнява действие и пренасочва, или връща страница/файл */
static void serve(sock_t sock, char *target, const char *referer) {
    char file[1100] = "", page[1100], loc[1200], head[1400], *body = NULL, *q = strchr(target, '?'), *dot;
    const char *p = target, *status = "200 OK", *to;
    size_t len = 0;
    int hl;

    if (q) *q++ = 0; else q = target + strlen(target);     /* делим адреса на път и параметри след "?" */
    while (*p == '/') p++;                                 /* махаме водещото "/" */

    pthread_mutex_lock(&lock);              /* само една нишка наведнъж пипа кошницата */
    to = run_action(p, q, referer, loc, sizeof loc);       /* дали е действие (add, rm, order...) */
    if (!to) {                              /* не е действие – търсим файл в папката site */
        if (!*p) p = "index.html";
        snprintf(file, sizeof file, SITE "/%s%s", p, strchr(p, '.') ? "" : ".html");   /* "cart" -> site/cart.html */
        if (!strstr(p, "..") && strncmp(p, "parts/", 6)) body = read_file(file, &len); /* забраняваме ".." и parts/ */
        if (!body) {                        /* няма такъв файл – грешка 404 */
            status = "404 Not Found";
            p = "404";
            snprintf(file, sizeof file, "404.html");
            body = strdup("404 - Страницата не е намерена");
        }
        snprintf(page, sizeof page, "%s", p);               /* име на страницата без ".html" */
        if ((dot = strstr(page, ".html"))) *dot = 0;
        if (!strcmp(mime(file), HTML)) {    /* HTML страниците минават през render (маркерите) */
            Buf b = {0};
            render(&b, body, &(Req){page, q}, 0);
            free(body);
            body = b.d;
            len = b.n;
        }
    }
    pthread_mutex_unlock(&lock);

    if (to)                                 /* пренасочване към друга страница */
        hl = snprintf(head, sizeof head, "HTTP/1.1 303 See Other\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", to);
    else                                    /* нормален отговор със съдържание */
        hl = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      status, mime(file), len);
    send_all(sock, head, (size_t)hl);
    send_all(sock, body, len);
    free(body);
}

/* Работи в отделна нишка за всяка връзка: чете заявката и праща отговор */
static void *worker(void *arg) {
    sock_t sock = (sock_t)(uintptr_t)arg;
    char req[4096], method[8], target[1024], referer[512] = "", *h;
    int n;
#ifdef _WIN32
    DWORD tv = 2000;                        /* най-много 2 секунди чакане за заявката */
#else
    struct timeval tv = {2, 0};
#endif

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
    n = (int)recv(sock, req, sizeof req - 1, 0);            /* четем заявката от браузъра */
    if (n > 0) {
        req[n] = 0;
        if ((h = strstr(req, "\r\nReferer:"))) sscanf(h + 10, " %511[^\r\n]", referer);   /* откъде идва потребителят */
        if (sscanf(req, "%7s %1023s", method, target) == 2 && !strcmp(method, "GET"))   /* "GET /cart HTTP/1.1" */
            serve(sock, target, referer);
    }
    shutdown(sock, 1);
    CLOSE(sock);
    return NULL;
}

/* Началото на програмата: пуска сървъра и приема връзки */
int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080, yes = 1;    /* порт 8080 или този от командния ред */
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY),
                               .sin_port = htons((unsigned short)port)};
    sock_t srv;

    if (access(SITE, F_OK)) {               /* проверка дали сме в правилната папка */
        fputs("Не намирам папката site. Стартирай програмата от главната папка на проекта.\n", stderr);
        return 1;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);       /* включва мрежата на Windows */
    SetConsoleOutputCP(CP_UTF8);            /* за да се виждат правилно българските букви */
#else
    signal(SIGPIPE, SIG_IGN);               /* програмата да не спира при прекъсната връзка */
#endif
    srv = socket(AF_INET, SOCK_STREAM, 0);  /* създаваме сокет */
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);   /* позволява бърз рестарт на същия порт */
    if (srv == INVALID_SOCKET || bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(srv, 32) < 0) {
        perror("server");                   /* напр. портът е зает */
        return 1;
    }
    printf("Sound of the Electricity работи на http://localhost:%d\n", port);
    fflush(stdout);

    for (;;) {                              /* безкраен цикъл: приемаме връзка и я даваме на нова нишка */
        sock_t c = accept(srv, NULL, NULL);
        pthread_t th;
        if (c == INVALID_SOCKET) continue;
        if (pthread_create(&th, NULL, worker, (void *)(uintptr_t)c) == 0) pthread_detach(th);
        else CLOSE(c);
    }
}
