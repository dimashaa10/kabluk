/*
    buttons.qc

    button and multiple button

    ---------------------------------------------------------------------
    Что исправлено в этой версии (подробности в комментариях на месте):

      1. "wait" -1 теперь действительно значит "никогда не возвращается".
         button_wait ставил nextthink = ltime + wait безусловно, а при
         wait = -1 это время в прошлом, то есть кнопка возвращалась на
         следующем кадре. См. button_wait.

      2. button_use и button_touch больше не перезаписывают self.enemy,
         если кнопка в этот момент едет или уже нажата. Раньше кнопка,
         нажатая во время хода, засчитывала отложенный SUB_UseTargets()
         другому игроку. См. button_use и button_idle.

      3. button_killed больше не оставляет кнопку нерасстреливаемой, если
         выстрел пришёлся на уже нажатую кнопку. См. button_killed.
    ---------------------------------------------------------------------

    only_touch (ключ в редакторе карт):

        0 - кнопка нажимается ТОЛЬКО клавишей использования (+use, по умолчанию E)
        1 - кнопка срабатывает ТОЛЬКО когда её задевает игрок, клавиша не работает
        2 - работает и касанием, и клавишей

    Важно: ключ должен быть объявлен полем (.float only_touch; в defs.qc),
    иначе движок выбросит его из .bsp как неизвестный.
*/

void() button_wait;
void() button_return;

/*
=================
button_idle

FALSE, если кнопка уже нажата (STATE_TOP) или ещё едет наверх (STATE_UP).
В обоих случаях new press надо игнорировать целиком - и движение, и запись
self.enemy: see button_use.

STATE_DOWN (кнопка едет обратно) idle считается: повторное нажатие посреди
возврата - это нормальное поведение кнопки, и оно честно запускает новый
цикл.
=================
*/
float() button_idle =
{
	if (self.state == STATE_UP || self.state == STATE_TOP)
		return FALSE;
	return TRUE;
};

void() button_wait =
{
	self.state = STATE_TOP;
	self.frame = 1;			// use alternate textures

	/* wait = -1 значит "не возвращается никогда", и раньше это не работало:
	 *
	 *     self.nextthink = self.ltime + self.wait;   // ltime + (-1) - это
	 *                                                // время в прошлом
	 *
	 * - то есть button_return вызывался на следующем же кадре, и кнопка
	 * приезжала назад. Теперь при -1 возврат просто не планируется: think
	 * остаётся прежним, nextthink = 0.
	 *
	 * Никакого SUB_Null здесь не нужно: движок обнуляет nextthink перед
	 * вызовом think, поэтому "не планировать" - это и есть "остановиться". */
	if (self.wait == -1)
		self.nextthink = 0;
	else
	{
		self.think = button_return;
		self.nextthink = self.ltime + self.wait;
	}

	/* self.enemy - тот, кто нажал. Он записан в button_use/button_touch
	 * ДО вызова button_fire, и ничто его с тех пор не меняет: см. там. */
	activator = self.enemy;
	SUB_UseTargets();
};

void() button_done =
{
	self.state = STATE_BOTTOM;
};

void() button_return =
{
	self.state = STATE_DOWN;
	SUB_CalcMove (self.pos1, self.speed, button_done);
	self.frame = 0;			// use normal textures

	if (self.health)
		self.takedamage = DAMAGE_YES;	// can be shot again
};

void() button_blocked =
{	// do nothing, just don't come all the way back out
};

void() button_fire =
{
	/* Оставлено как защита: все три вызывающих уже проверяют button_idle(),
	 * но button_fire - единственное место, которое реально двигает кнопку,
	 * и оно не должно зависеть от аккуратности вызывающих. */
	if (!button_idle ())
		return;

	sound (self, CHAN_VOICE, self.noise, 1, ATTN_NORM);

	self.state = STATE_UP;
	SUB_CalcMove (self.pos2, self.speed, button_wait);
};

void() button_use =
{
	// 1 = только касание: отказ и клавише, и любому триггеру,
	// который дёргает кнопку через .use по targetname
	if (self.only_touch == 1)
		return;

	/* Проверка состояния - ДО записи self.enemy.
	 *
	 * Было:
	 *     self.enemy = activator;   // записали того, кто нажал сейчас
	 *     button_fire ();           // ...а он может сразу выйти: кнопка едет
	 *
	 * Если нажать кнопку, пока она едет наверх или уже нажата, self.enemy
	 * перезаписывался, и отложенный SUB_UseTargets() в button_wait засчитывал
	 * нажатие уже ДРУГОМУ игроку. В кооперативе это видно на дверях с ключом:
	 * ключ проверяется у того, кто "нажал", а кнопку нажал другой. */
	if (!button_idle ())
		return;

	self.enemy = activator;		// кто нажал - его и запомним для SUB_UseTargets
	button_fire ();
};

void() button_touch =
{
	if (other.classname != "player")
		return;

	// 0 = только клавиша: касание игнорируется
	if (self.only_touch == 0)
		return;

	// та же причина, что и в button_use: не перезаписывать self.enemy,
	// если нажатие всё равно будет проигнорировано
	if (!button_idle ())
		return;

	self.enemy = other;
	button_fire ();
};

void() button_killed =
{
	/* Стреляемая кнопка. Три вещи, которые здесь важны:
	 *
	 * 1. Выстрел по кнопке, которая УЖЕ нажата или едет, не должен
	 *    выключать ей takedamage: раньше takedamage = DAMAGE_NO ставился
	 *    всегда, а возвращался только в button_return - то есть кнопка с
	 *    wait = -1 оставалась нерасстреливаемой навсегда.
	 * 2. health восстанавливается, чтобы кнопку можно было расстрелять
	 *    снова после возврата (это делает button_return).
	 * 3. self.enemy - тот, кто выстрелил: его получит SUB_UseTargets. */
	self.health = self.max_health;

	if (!button_idle ())
		return;			// движок уже её двигает: выстрел ничего не меняет

	self.enemy = damage_attacker;
	self.takedamage = DAMAGE_NO;	// wil be reset upon return
	button_fire ();
};



/*QUAKED func_button (0 .5 .8) ?
When a button is touched, it moves some distance in the direction of it's angle, triggers all of it's targets, waits some time, then returns to it's original position where it can be triggered again.

"angle"		determines the opening direction
"target"	all entities with a matching targetname will be used
"speed"		override the default 40 speed
"wait"		override the default 1 second wait (-1 = never return)
"lip"		override the default 4 pixel lip remaining at end of move
"health"	if set, the button must be killed instead of touched
"only_touch"	0 = only the use key (E), 1 = only touching, 2 = both (a missing key means 0)
"sounds"
0) steam metal
1) wooden clunk
2) metallic click
3) in-out
*/
void() func_button =
{
local float		can_touch;

	if (self.sounds == 0)
	{
		precache_sound ("buttons/airbut1.wav");
		self.noise = "buttons/airbut1.wav";
	}
	if (self.sounds == 1)
	{
		precache_sound ("buttons/switch21.wav");
		self.noise = "buttons/switch21.wav";
	}
	if (self.sounds == 2)
	{
		precache_sound ("buttons/switch02.wav");
		self.noise = "buttons/switch02.wav";
	}
	if (self.sounds == 3)
	{
		precache_sound ("buttons/switch04.wav");
		self.noise = "buttons/switch04.wav";
	}

	can_touch = FALSE;
	if (self.only_touch == 1 || self.only_touch == 2)
		can_touch = TRUE;

	SetMovedir ();

	self.movetype = MOVETYPE_PUSH;
	self.solid = SOLID_BSP;
	setmodel (self, self.model);

	self.blocked = button_blocked;

	// .use ставим всегда: button_use сам решает, можно ли нажимать клавишей.
	// Так и триггер, дёргающий кнопку по targetname, получает тот же отказ.
	self.use = button_use;

	if (self.health)
	{
		/* "health" главнее only_touch: такую кнопку карта велит расстрелять,
		 * и она не получает .touch вообще. Движок тоже отказывает ей в
		 * нажатии клавишей (он не трогает кнопки с health > 0). */
		self.max_health = self.health;
		self.th_die = button_killed;
		self.takedamage = DAMAGE_YES;	// стреляемая кнопка: только выстрел
	}
	else if (can_touch)
	{
		self.touch = button_touch;
	}
	// иначе .touch остаётся пустым: движок даже не будет пытаться вызвать касание

	if (!self.speed)
		self.speed = 40;
	if (!self.wait)
		self.wait = 1;
	if (!self.lip)
		self.lip = 4;

	self.state = STATE_BOTTOM;

	self.pos1 = self.origin;
	self.pos2 = self.pos1 + self.movedir*(fabs(self.movedir*self.size) - self.lip);
};
