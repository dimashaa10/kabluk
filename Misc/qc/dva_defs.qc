/*
    dva_defs.qc -- объявления для "Dynamic view actions"

    ЭТОТ ФАЙЛ НУЖНО ЦЕЛИКОМ ВСТАВИТЬ В defs.qc (в конец файла), а не
    подключать отдельно.

    ПОЧЕМУ ИМЕННО В defs.qc, А НЕ "ПРОСТО ДОБАВИТЬ dynview.qc В progs.src"

    QuakeC компилирует файлы строго по порядку из progs.src, и имя, которое
    ещё не встретилось, — ошибка "Unknown value". defs.qc стоит первым, поэтому
    всё, что здесь объявлено, известно ВСЕМ остальным файлам: items.qc (где
    вызывается DVA_Play), world.qc (где precache_model (DVA_ACTIONS_MODEL)) и
    любому следующему.

    Если положить эти строки в dynview.qc, то файл придётся ставить в
    progs.src ПЕРЕД items.qc — и любая перестановка сломает сборку снова.
    Тела функций при этом могут лежать где угодно (хоть в конце): объявления
    здесь уже есть.

    ЧТО ЗДЕСЬ ЧТО

    Поля (их пишет и читает движок, см. Quake/pr_edict.c, progs.h):
        dvamodel   - имя модели действия (string_null = анимации нет)
        dvastart   - когда началась (time)
        dvafirst   - первый кадр действия в модели, С НУЛЯ
        dvaframes  - сколько кадров играть
        dvafps     - кадров в секунду (0 = 10)
        dvaend     - когда закончится (для блокировки стрельбы)

    Константы — модель и её отрезки. Кадры в QuakeC считаются С ЕДИНИЦЫ.

    Прототипы — чтобы items.qc и world.qc могли пользоваться DVA_Play раньше,
    чем компилятор дойдёт до её тела. Сигнатура обязана совпадать с
    определением в dynview.qc символ в символ, иначе часть компиляторов QC
    выдаст несовпадение типов.
*/

/* ---- поля энтити ---- */
.string dvamodel;
.float dvastart;
.float dvafirst;
.float dvaframes;
.float dvafps;
.float dvaend;

/* ---- модель действий и её отрезки (кадры с единицы) ---- */
string DVA_ACTIONS_MODEL = "progs/v_actions.mdl";
float DVA_ACT_MEDKIT_FIRST = 1;		// средняя аптечка (+25 hp): кадры 1..11
float DVA_ACT_MEDKIT_FRAMES = 11;
float DVA_ACT_SMALLKIT_FIRST = 12;	// мелкая аптечка (+15 hp): кадры 12..23
float DVA_ACT_SMALLKIT_FRAMES = 12;
float DVA_DEFAULT_FPS = 10;			// когда в DVA_Play передали 0

/* ---- прототипы ---- */
float(entity p, string mdl, float first, float frames, float fps) DVA_Play;
void(entity p) DVA_Stop;
