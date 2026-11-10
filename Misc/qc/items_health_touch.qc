/*
    items_health_touch.qc -- health_touch с анимацией для аптечек

    Это ТВОЯ функция целиком. Изменений ровно два (переменная `pickup` и
    вызов DVA_Play в конце), они помечены `// >>> DVA`. Всё остальное —
    твой исходный код, символ в символ.

    ЧТО ИМЕННО ИЗМЕНИЛОСЬ

    1. Новая локальная переменная `pickup` в начале функции.
       Зачем: после T_Heal нужно знать, ЧТО именно подобрали, а `self.healtype`
       обнулять нельзя — ниже по коду он ещё используется для мега-хелса.

    2. После `stuffcmd (other, "bf\n");` вызывается DVA_Play.
       Место выбрано не случайно:
         * ПОСЛЕ T_Heal — если здоровье полное, T_Heal вернул 0, функция уже
           вышла на `return`, и анимации не будет. Это правильно: подобрал
           пустую аптечку — ничего не произошло и показывать нечего;
         * ПОСЛЕ звука и stuffcmd — анимация начинается там же, где в
           оригинале начинается эффект подбора;
         * ДО `self.model = string_null` — не важно для движка, но так
           видно, что анимация относится к подбору, а не к удалению предмета.

    ПОЧЕМУ УСЛОВИЕ ПО `pickup` (бывший `self.healtype`)

    В твоём item_health:
        spawnflags H_ROTTEN -> healamount 15,  healtype 0
        иначе (обычная)     -> healamount 25,  healtype 1
        spawnflags H_MEGA   -> healamount 100, healtype 2

    Отрезки в v_actions.mdl (кадры с единицы):
        кадры  1..11  -- средняя аптечка (+25 hp)  DVA_ACT_MEDKIT_*
        кадры 12..23  -- мелкая аптечка  (+15 hp)  DVA_ACT_SMALLKIT_*

    Поэтому веток две: `pickup == 0` играет 12..23, `pickup == 1` — 1..11.
    Мега-хелс (healtype 2) остаётся без анимации: кадров для него нет.
    Когда сделаешь — добавишь ещё одну ветку рядом, по образцу.

    ЧТО ЕЩЁ НУЖНО, КРОМЕ ЭТОЙ ФУНКЦИИ

    1. Объявления в defs.qc (см. Misc/qc/dva_defs.qc):
           .string dvamodel;
           .float dvastart, dvafirst, dvaframes, dvafps;
           .float dvaend;
    2. Предзагрузка модели в world-спавне (иначе сервер её не найдёт и
       анимации не будет, но и не упадёт):
           precache_model (DVA_ACTIONS_MODEL);
    3. Файл dynview.qc (DVA_Play и константы) — в моде.
    4. Если хочешь, чтобы во время анимации нельзя было стрелять — в
       PlayerPreThink:
           if (self.dvaend > time)
               self.button0 = 0;

    Почему `pickup` не нужен, если анимация только у средней: он понадобится,
    когда добавишь кадры для мега-хелса — тогда ветку не придётся
    переписывать. Если не нужен — убери переменную и оставь прямой вызов.
*/

void() health_touch =
{
	local	string	s;
	local	float	pickup;		// >>> DVA: какой тип аптечки подобрали

	if (deathmatch_supermode())
		if (other.invincible_time > 0)
			return;

	if (other.classname != "player")
		return;

	pickup = self.healtype;		// >>> DVA: запоминаем ДО возможных изменений

	if (self.healtype == 2) // Megahealth?  Ignore max_health...
	{
		if (other.health >= 250)
			return;
		if (!T_Heal(other, self.healamount, 1))
			return;
	}

	else
	{
		if (!T_Heal(other, self.healamount, 0))
			return;
	}

	sprint(other, PRINT_LOW, LOC_ITEM_PICKUP_HEALTH);
	s = ftos(self.healamount);
	sprint(other, PRINT_LOW, s);
	sprint(other, PRINT_LOW, LOC_ITEM_TYPE_HEALTH);
	sprint(other, PRINT_LOW, "\n");

	// health touch sound
	sound(other, CHAN_ITEM, self.noise, 1, ATTN_NORM);

	stuffcmd (other, "bf\n");

	// >>> DVA: анимация подбора. Отрезок кадров -- свой у каждой аптечки:
	// healtype 0 -- мелкая (+15 hp), кадры 12..23 (12 кадров);
	// healtype 1 -- средняя (+25 hp), кадры 1..11 (11 кадров);
	// healtype 2 -- мега (+100 hp), кадров пока нет.
	if (pickup == 0)
		DVA_Play (other, DVA_ACTIONS_MODEL,
		          DVA_ACT_SMALLKIT_FIRST, DVA_ACT_SMALLKIT_FRAMES, 10);
	else if (pickup == 1)
		DVA_Play (other, DVA_ACTIONS_MODEL,
		          DVA_ACT_MEDKIT_FIRST, DVA_ACT_MEDKIT_FRAMES, 10);

	self.model = string_null;
	self.solid = SOLID_NOT;

	// Megahealth = rot down the player's super health
	if (self.healtype == 2 && !deathmatch_supermode())
	{
		other.items = other.items | IT_SUPERHEALTH;
		self.owner = other;
		self.nextthink = time + 5;
		self.think = item_megahealth_rot;
	}

	else
	{
		if (deathmatch_item_respawn())		// deathmatch 2 is the silly old rules
		{
			self.nextthink = time + 20;
			self.think = SUB_regen;
		}
	}

	activator = other;
	
	
	SUB_UseTargets();				// fire all targets / killtargets
};
