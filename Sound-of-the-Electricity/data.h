/* ===== data.h – данните на магазина (цени и опции за къстъм китарата) =====
   Тук се сменят цените и се добавят нови опции. В main.c не е нужно да се пипа. */

/* Една опция: поле във формата, стойност във формата, име за показване, цена в € */
typedef struct { const char *name, *value, *label; int price; } Choice;

/* Полетата на къстъм формата: име във формата и етикет за показване */
static const struct { const char *name, *label; } FIELD[] = {
    {"brand", "Марка"}, {"body", "Модел"}, {"color", "Цвят"}, {"head", "Хедсток"},
    {"tuners", "Колчета"}, {"pickups", "Пикъпи"}, {"type", "Тип пикъпи"}};

/* Всички възможни избори и цената им в евро. Тук се сменят цените на къстъм китарата.
   Първите 7 полета са задължителни, останалите (case, strap...) са по желание. */
static const Choice CHOICES[] = {
    {"brand", "gibson", "Gibson", 0}, {"brand", "fender", "Fender", 0}, {"brand", "jackson", "Jackson", 0},
    {"body", "gibson-lespaul", "Les Paul", 2600}, {"body", "gibson-sg", "SG", 1900}, {"body", "gibson-v", "Flying V", 2300},
    {"body", "fender-strat", "Stratocaster", 1700}, {"body", "fender-tele", "Telecaster", 1600},
    {"body", "jackson-dinky", "Dinky", 1300}, {"body", "jackson-rhoads", "Rhoads", 1450},
    {"body", "jackson-superstrat", "Superstrat", 1350},
    {"color", "black", "Черно", 0}, {"color", "white", "Бяло", 0}, {"color", "red", "Червено", 80},
    {"color", "blue", "Синьо", 80}, {"color", "sunburst", "Sunburst", 150}, {"color", "custom", "Цвят по избор", 250},
    {"head", "gibson-std", "Стандартен Gibson", 0}, {"head", "fender-strat", "Stratocaster", 0},
    {"head", "fender-tele", "Telecaster", 0}, {"head", "jackson-classic", "Класически Jackson", 0},
    {"head", "jackson-reverse", "Reverse Jackson", 60}, {"head", "jackson-arrow", "Arrow", 60},
    {"tuners", "normal", "Стандартни", 0}, {"tuners", "locking", "Заключващи", 120},
    {"pickups", "sss", "SSS", 0}, {"pickups", "ssh", "SSH", 40}, {"pickups", "hsh", "HSH", 60},
    {"pickups", "hh", "HH", 80}, {"pickups", "p90", "2× P90", 90},
    {"type", "passive", "Пасивни", 0}, {"type", "active", "Активни", 180},
    {"case", "1", "Твърд калъф", 220}, {"strap", "1", "Каиш", 25},
    {"picks", "1", "Персонализирани медиатори (12 бр.)", 15}, {"cable", "1", "Кабел 3 м", 20},
    {"strings", "1", "3 комплекта резервни струни", 30}, {"tuner", "1", "Clip-on тунер", 20},
    {"setup", "1", "Професионален сетъп", 60}};
