/*
    monster_pocoyniy67 -- NPC, который не дерётся.

    ЧТО ОН ДЕЛАЕТ

      1. Стоит на месте (idle), иногда бесцельно бежит в случайную сторону.
      2. Игрока не атакует вообще: у него нет ни th_attack, ни поиска врага,
         ни стрельбы. Он даже не смотрит на игрока, пока в него не попали.
      3. Когда в него попали -- убегает ОТ ИГРОКА ровно 3 секунды.
         Попали ещё раз -- три секунды начинаются заново.
      4. Через 3 секунды останавливается и снова живёт своей жизнью
         (постоит 1-3 секунды, потом может побежать в случайную сторону).
      5. 50 здоровья, при смерти проигрывает анимацию 31..47 и остаётся
         лежать трупом.

    МОДЕЛЬ: progs/pocoyniy67.mdl, кадры СЧИТАЮТСЯ С ЕДИНИЦЫ:

        кадры  1..20  -- idle,           20 кадров, 8 к/с  (цикл 2.5 с)
        кадры 21..30  -- передвижение,   10 кадров, 10 к/с (цикл 1.0 с)
        кадры 31..47  -- смерть,         17 кадров, 10 к/с (1.7 с, без цикла)

    ВАЖНО: в QuakeC кадры модели считаются С НУЛЯ, а вы называете их с
    единицы (как в любом редакторе). Поэтому все константы ниже -- с
    единицы, а переводит их в нумерацию модели ОДНА функция, poc_setanim()
    (строка `f0 = first - 1;`). Если модель на самом деле начинается не с
    того кадра, правится одно место, а не двадцать.

    КАК ЭТО ПОДКЛЮЧИТЬ (три шага)

      1. Положить файл в мод и добавить строку `monster_pocoyniy67.qc`
         в progs.src -- в любое место ПОСЛЕ defs.qc. Все имена, которые
         этому файлу нужны, объявлены либо в нём самом выше по тексту,
         либо в стандартном defs.qc, поэтому порядок относительно других
         файлов мода не важен.
      2. Пересобрать progs.dat.
      3. Поставить монстра на карту: entity с classname
         "monster_pocoyniy67" (обычный механизм спавна по имени класса)
         или вызвать monster_pocoyniy67 (); из своего QC-кода.

         Для TrenchBroom рядом лежит описание entity:
         Misc/fgd/monster_pocoyniy67.fgd (модель, размер, angle,
         spawnflags). Игра этот файл не читает -- он только для редактора.

    ПОЧЕМУ ЗДЕСЬ НЕТ НИ ОДНОГО БУДЕТ-ЛИ-ЭТО-РАБОТАТЬ-ЗАВИСИМОСТИ ОТ МОДА

    Файл специально не трогает чужие функции и переменные (никаких
    monster_start(), walkmonster_start(), ChangeYaw(), T_Damage,
    AttackFinished, sound()) -- только стандартные builtin'ы самого
    движка: precache_model, setmodel, setsize, droptofloor, walkmove,
    vectoyaw, find, random, floor. Поворот делается своим кодом
    (poc_turn), потому что ChangeYaw в разных модах объявлен по-разному:
    где-то это функция в monsters.qc, где-то builtin движка. Свой поворот
    работает в обоих случаях и не требует править defs.qc.

    ГЛАВНАЯ ТОНКОСТЬ: ЧЕМ ДВИГАТЬСЯ

    Монстр с MOVETYPE_STEP сам не ходит: движок для него делает только
    гравитацию (sv_phys.c, SV_Physics_Step). Ходить он должен вызовом
    walkmove (yaw, dist) -- это builtin, который двигает self на dist
    юнитов в направлении yaw и умеет ступеньки и склоны (sv_move.c,
    SV_movestep).

    walkmove возвращает 0, если идти некуда (стена, край платформы --
    с обрыва монстр не спрыгнет). На этот 0 мы и реагируем: выбираем
    другое направление.

    walkmove ещё и требует FL_ONGROUND (pr_cmds.c, PF_walkmove: без
    ONGROUND возвращает 0 и ничего не делает). Поэтому, пока монстр
    падает на пол после спавна, think ничего не делает, кроме ожидания:
    приземление и выставление FL_ONGROUND -- работа движка.

    Почему think раз в 0.1 секунды: это обычный шаг для монстров Quake,
    и при скорости 120 юнитов в секунду он даёт 12 юнитов за шаг.
*/

/* ------------------------------------------------------------------ */
/*  Поля. Свои, с префиксом poc_, чтобы не столкнуться с полями мода.  */
/* ------------------------------------------------------------------ */

.float  poc_state;      // POC_STAND или POC_WALK -- что делает сейчас
.float  poc_until;      // до какого времени длится текущее действие
.float  poc_flee;       // до какого времени убегаем (0 = не убегаем)
.float  poc_afirst;     // первый кадр текущего отрезка, С НУЛЯ (как в модели)
.float  poc_aframes;    // длина текущего отрезка в кадрах
.float  poc_afps;       // кадров в секунду для текущего отрезка
.float  poc_astart;     // когда текущий отрезок начался
.vector poc_runfrom;    // точка, ОТ которой убегаем

/* ------------------------------------------------------------------ */
/*  Константы.                                                         */
/* ------------------------------------------------------------------ */

float POC_HEALTH       = 50;    // здоровья

float POC_IDLE_FIRST   = 1;     // idle: кадры 1..20 (с единицы)
float POC_IDLE_FRAMES  = 20;
float POC_IDLE_FPS     = 8;

float POC_MOVE_FIRST   = 21;    // передвижение: кадры 21..30
float POC_MOVE_FRAMES  = 10;
float POC_MOVE_FPS     = 10;

float POC_DIE_FIRST    = 31;    // смерть: кадры 31..47
float POC_DIE_FRAMES   = 17;
float POC_DIE_FPS      = 10;

float POC_STAND        = 0;     // значения poc_state
float POC_WALK         = 1;

float POC_THINK        = 0.1;   // шаг think, секунды
float POC_FLEE_TIME    = 3;     // сколько убегать после попадания
float POC_WANDER_SPEED = 120;   // юнитов в секунду, бесцельное бегание
float POC_FLEE_SPEED   = 250;   // юнитов в секунду, убегание
float POC_YAW_SPEED    = 300;   // градусов в секунду, скорость поворота

/* ------------------------------------------------------------------ */
/*  Анимация.                                                          */
/* ------------------------------------------------------------------ */

/*
=================
poc_setanim

Переключить self на отрезок кадров `first` (С ЕДИНИЦЫ), длиной `frames`
кадров, со скоростью `fps` кадров в секунду.

Если этот же отрезок уже играет -- НЕ перезапускаем: иначе анимация
дёргалась бы на каждом think.
=================
*/
void(float first, float frames, float fps) poc_setanim =
{
	local float f0;

	if (frames < 1)
		frames = 1;
	if (fps <= 0)
		fps = 1;

	f0 = first - 1;			// QuakeC считает с единицы, модель -- с нуля
	if (f0 < 0)
		f0 = 0;

	if (self.poc_afirst == f0 && self.poc_aframes == frames && self.poc_afps == fps)
		return;				// этот отрезок уже играет

	self.poc_afirst  = f0;
	self.poc_aframes = frames;
	self.poc_afps    = fps;
	self.poc_astart  = time;
};

/*
=================
poc_loopframe

Поставить self.frame в кадр, который должен быть виден СЕЙЧАС, по часам
(time - poc_astart). Кадр зациклен: после последнего идёт первый.

Считается по часам, а не «+1 кадр за think»: так анимация не зависит от
того, сколько раз и когда движок позвал think.
=================
*/
void() poc_loopframe =
{
	local float f;

	if (self.poc_aframes <= 0 || self.poc_afps <= 0)
		return;

	f = floor ((time - self.poc_astart) * self.poc_afps);
	// остаток от деления на длину отрезка
	f = f - self.poc_aframes * floor (f / self.poc_aframes);
	self.frame = self.poc_afirst + f;
};

/*
=================
poc_die_frame

То же самое, но БЕЗ цикла: на последнем кадре анимация замирает.
Так смерть не начнёт проигрываться заново.
=================
*/
void() poc_die_frame =
{
	local float f;

	if (self.poc_aframes <= 0 || self.poc_afps <= 0)
		return;

	f = floor ((time - self.poc_astart) * self.poc_afps);
	if (f > self.poc_aframes - 1)
		f = self.poc_aframes - 1;		// застываем на последнем кадре
	self.frame = self.poc_afirst + f;
};

/* ------------------------------------------------------------------ */
/*  Поворот.                                                           */
/* ------------------------------------------------------------------ */

/*
=================
poc_turn

Повернуть self в сторону self.ideal_yaw не быстрее чем на
POC_YAW_SPEED градусов за think.

Свой код вместо ChangeYaw: в одних модах ChangeYaw -- функция в
monsters.qc, в других -- builtin движка. Своя версия не зависит ни от
того, ни от другого и не требует ничего добавлять в defs.qc.

Угол держим в 0..360 сами (вместо anglemod, который тоже объявлен
по-разному), чтобы разница углов всегда считалась правильно.
=================
*/
void() poc_turn =
{
	local float d;

	self.angles_y = self.angles_y - 360 * floor (self.angles_y / 360);
	self.ideal_yaw = self.ideal_yaw - 360 * floor (self.ideal_yaw / 360);
	d = self.ideal_yaw - self.angles_y;
	if (d > 180)
		d = d - 360;
	if (d < -180)
		d = d + 360;
	if (d > POC_YAW_SPEED)
		d = POC_YAW_SPEED;
	if (d < -POC_YAW_SPEED)
		d = -POC_YAW_SPEED;
	self.angles_y = self.angles_y + d;
};

/* ------------------------------------------------------------------ */
/*  Мозги.                                                             */
/* ------------------------------------------------------------------ */

/*
=================
poc_think

Один think раз в POC_THINK секунд. Порядок проверок важен:

    1. мёртв        -- только анимация смерти, больше ничего;
    2. не на земле  -- ждём, пока движок уронит на пол;
    3. убегаем      -- 3 секунды после попадания;
    4. обычная жизнь-- стоим или бесцельно идём.

Убегание идёт ПЕРЕД обычным поведением: это и есть «испугался».
=================
*/
void() poc_think =
{
	// --- 1. мёртвый: доигрываем смерть и замираем на последнем кадре ---
	if (self.health <= 0)
	{
		poc_die_frame ();
		if (self.poc_afps > 0 &&
		    time < self.poc_astart + self.poc_aframes / self.poc_afps)
			self.nextthink = time + POC_THINK;	// ещё не доиграли
		return;
	}

	// --- 2. ещё падаем на пол: walkmove без FL_ONGROUND не работает ---
	if (!(self.flags & FL_ONGROUND))
	{
		self.nextthink = time + POC_THINK;
		return;
	}

	self.velocity = '0 0 0';		// двигаемся только через walkmove

	// --- 3. убегаем ---
	if (self.poc_flee > time)
	{
		poc_setanim (POC_MOVE_FIRST, POC_MOVE_FRAMES, POC_MOVE_FPS);
		poc_loopframe ();

		// Бежим от ЖИВОГО игрока, а не от точки, где он был: если он идёт
		// за нами, направление обновляется каждый think. Игрок умер --
		// остаётся последняя точка, от неё и убегаем.
		if (self.enemy != world && self.enemy.health > 0)
			self.poc_runfrom = self.enemy.origin;

		self.ideal_yaw = vectoyaw (self.origin - self.poc_runfrom);
		poc_turn ();

		if (!walkmove (self.ideal_yaw, POC_FLEE_SPEED * POC_THINK))
		{
			// упёрлись в угол -- в следующий think попробуем чуть в
			// сторону, но всё равно примерно от игрока
			self.ideal_yaw = self.ideal_yaw - 45 + random () * 90;
		}

		self.nextthink = time + POC_THINK;
		return;
	}

	// --- 4. обычная жизнь: постоять / бесцельно побегать ---
	// Одно действие длится 1-3 секунды, потом бросаем монетку:
	// стояли -- с вероятностью 1/2 пойдём; шли -- встанем.
	if (time >= self.poc_until)
	{
		if (self.poc_state == POC_STAND && random () < 0.5)
		{
			self.poc_state = POC_WALK;
			self.ideal_yaw = random () * 360;	// куда глаза глядят
			self.poc_until = time + 1 + random () * 2;
		}
		else
		{
			self.poc_state = POC_STAND;
			self.poc_until = time + 1 + random () * 2;
		}
	}

	if (self.poc_state == POC_WALK)
	{
		poc_setanim (POC_MOVE_FIRST, POC_MOVE_FRAMES, POC_MOVE_FPS);
		poc_loopframe ();
		poc_turn ();

		if (!walkmove (self.ideal_yaw, POC_WANDER_SPEED * POC_THINK))
		{
			// стена или край -- в следующий раз попробуем в другую сторону
			self.ideal_yaw = random () * 360;
		}
	}
	else
	{
		poc_setanim (POC_IDLE_FIRST, POC_IDLE_FRAMES, POC_IDLE_FPS);
		poc_loopframe ();
	}

	self.nextthink = time + POC_THINK;
};

/*
=================
poc_pain

Вызывается модом из T_Damage. Здесь и только здесь монстр узнаёт, что в
него попали: он не ищет игрока сам, поэтому «атаковать» не может.

Что делаем: запоминаем игрока и включаем убегание на POC_FLEE_TIME
секунд от текущего момента. Повторное попадание просто продлевает.
=================
*/
void() poc_pain =
{
	if (self.health <= 0)
		return;					// добивание не должно менять анимацию

	self.enemy = find (world, classname, "player");
	if (self.enemy == world)
		return;					// стрелял не игрок -- убегать не от кого

	self.poc_runfrom = self.enemy.origin;
	self.poc_flee = time + POC_FLEE_TIME;

	// После убегания постоит 1-3 секунды, а не побежит сразу.
	self.poc_state = POC_STAND;
	self.poc_until = self.poc_flee + 1 + random () * 2;
};

/*
=================
poc_die

Вызывается модом, когда здоровье кончилось. Ничего не взрывается и не
разлетается на гибы: проигрываем кадры 31..47 и остаёмся лежать.

takedamage = DAMAGE_NO -- иначе труп можно «добивать» и анимация смерти
начнётся заново. solid = SOLID_NOT -- труп не должен быть препятствием.
movetype не меняем: MOVETYPE_STEP сам продолжит держать труп на полу.
=================
*/
void() poc_die =
{
	self.takedamage = DAMAGE_NO;
	self.solid = SOLID_NOT;
	self.velocity = '0 0 0';
	self.enemy = world;
	self.poc_flee = 0;
	self.poc_state = POC_STAND;

	poc_setanim (POC_DIE_FIRST, POC_DIE_FRAMES, POC_DIE_FPS);
	self.frame = self.poc_afirst;		// первый кадр смерти сразу
	self.nextthink = time + POC_THINK;	// дальше анимацию ведёт poc_think
};

/* ------------------------------------------------------------------ */
/*  Спавн.                                                             */
/* ------------------------------------------------------------------ */

/*
=================
monster_pocoyniy67

Функция называется так же, как classname: карта спавнит её по имени.
=================
*/
void() monster_pocoyniy67 =
{
	precache_model ("progs/pocoyniy67.mdl");

	self.classname = "monster_pocoyniy67";
	self.solid = SOLID_SLIDEBOX;		// игрок не проходит сквозь него
	self.movetype = MOVETYPE_STEP;		// физика «ходячего» (гравитация движка)
	self.flags = self.flags | FL_MONSTER;
	self.health = POC_HEALTH;
	self.takedamage = DAMAGE_YES;
	self.th_pain = poc_pain;
	self.th_die = poc_die;
	self.think = poc_think;			// что движок зовёт по nextthink

	setmodel (self, "progs/pocoyniy67.mdl");
	setsize (self, '-16 -16 -24', '16 16 32');	// как у игрока

	self.origin_z = self.origin_z + 1;	// не застрять в полу при спавне
	droptofloor ();				// упасть на пол до старта

	self.ideal_yaw = self.angles_y;
	self.yaw_speed = POC_YAW_SPEED;

	self.poc_state = POC_STAND;
	self.poc_until = time + 1 + random () * 2;	// сначала постоит
	self.poc_flee = 0;

	self.nextthink = time + POC_THINK;
};

/* ================= пример на карте =================

   entity с classname "monster_pocoyniy67" и origin на полу.
   Оружие он не подбирает, предметы не трогает, стрелять не умеет.

   ================================================== */
