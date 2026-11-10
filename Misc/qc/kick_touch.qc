/*
    kick_touch

    Пнуть лежащий гиб. Всё как в вашей версии, плюс то, ради чего это
    переписано: гиб начинает КРУТИТЬСЯ.

    ---------------------------------------------------------------------
    ПОЧЕМУ ОН У ВАС НЕ КРУТИЛСЯ

    В QuakeC за вращение отвечает отдельное поле - .avelocity (угловая
    скорость, градусы в секунду). Движок сам интегрирует её:

        angles += avelocity * frametime          (sv_phys.c:1488)

    но только в SV_Physics_Toss(), то есть для MOVETYPE_(BOUNCE|TOSS|FLY),
    и только ПОКА ЭНТИТИ НЕ НА ЗЕМЛЕ:

        if (flags & FL_ONGROUND)
            return;                              (sv_phys.c:1473)

    А когда гиб приземляется, движок гасит не только скорость, но и
    вращение:

        VectorCopy (vec3_origin, ent->v.velocity);
        VectorCopy (vec3_origin, ent->v.avelocity);   (sv_phys.c:1516)

    Вы снимали FL_ONGROUND (правильно - иначе движок вообще не стал бы его
    двигать), но avelocity не выставляли, поэтому гиб просто летел
    поступательно, не вращаясь.

    ---------------------------------------------------------------------
    ЧТО СДЕЛАНО

    1. При пинке выставляется случайная угловая скорость по трём осям -
       быстро, но не бешено: 400..800 град/с. Каждая ось независимо, поэтому
       каждый гиб вертится по-своему.
    2. Убрана возможность пинать один и тот же гиб каждый кадр: касание
       player-touch срабатывает, пока гиб в его bbox, и прежняя версия
       напихивала импульс и звук десятки раз в секунду. Теперь пинается
       только «спокойный» гиб (fabs вместо vlen: vlen есть не во всех
       defs.qc, а fabs - стандартный builtin).
    3. Звук играется на гибе, а не на игроке: sound(other, CHAN_ITEM, ...)
       ставил звук в точку игрока и мог срезать его же звуки подбора
       предметов (оба на CHAN_ITEM).
    4. self.groundentity = world - чтобы гиб не остался «стоящим» на том, с
       чего его скинули.

    ---------------------------------------------------------------------
    ЧТО НЕЛЬЗЯ СДЕЛАТЬ ОДНИМ avelocity

    Крутиться ЛЁЖА гиб не может: при касании земли движок обнуляет
    avelocity (строка 1516 выше). Так что гиб вертится в полёте и замирает,
    когда уляжется - это как раз ванильное поведение Quake.
    Если всё-таки нужно, чтобы он крутился и на полу, это отдельный think:

        void() gib_spin_think =
        {
            if (!self.flags & FL_ONGROUND)   // на земле
                return;                       // think кончился, и хорошо
            self.avelocity = '0 0 ' ...;      // и снять ONGROUND снова -
                                              // движок снова начнёт крутить
            self.nextthink = time + 0.1;
        };

    но тогда гиб будет ползти по полу от интегрирования скорости, поэтому это
    уже не «ванильно», и я этого не включал.
*/

void() kick_touch;

void() kick_touch =
{
	local vector ang;
	local vector dir;
	local vector impulse;
	local float  kick;
	local float  spin;

	if (other.classname != "player")
		return;
	if (other.health <= 0)
		return;

	/* Касание срабатывает каждый кадр, пока гиб внутри bbox игрока. Пинаем
	 * только тот, который лежит спокойно: иначе за одно движение мышью
	 * набегает десяток импульсов и десяток звуков. */
	if (fabs(self.velocity_x) > 150 || fabs(self.velocity_y) > 150 ||
	    self.velocity_z > 20)
		return;

	// Направление взгляда, без наклона вверх/вниз.
	ang = other.angles;
	ang_x = 0;
	makevectors(ang);
	dir = v_forward;
	dir_z = 0;
	dir = normalize(dir);

	kick = 80 + random() * 80;

	impulse_x = dir_x * kick + other.velocity_x * 0.35;
	impulse_y = dir_y * kick + other.velocity_y * 0.35;
	impulse_z = 120 + random() * 10;

	// Небольшой бонус, если игрок сам подпрыгивает.
	if (other.velocity_z > 0)
		impulse_z = impulse_z + other.velocity_z * 0.2;

	/* Снять "стою на земле" - без этого движок в SV_Physics_Toss выходит
	 * на первой же проверке и гиб не двинется вообще. */
	self.flags = self.flags - (self.flags & FL_ONGROUND);
	self.groundentity = world;

	/* Сила вращения растёт вместе с силой пинка: лёгкий толчок - гиб
	 * кувыркается медленно, сильный - быстро. */
	spin = 400 + kick * 3;			// примерно 640..880 град/с
	self.avelocity_x = (random() - 0.5) * spin;
	self.avelocity_y = (random() - 0.5) * spin;
	self.avelocity_z = (random() - 0.5) * spin;

	self.velocity = self.velocity + impulse;

	// Звук - на гибе: он и есть то, что пнули, и CHAN_ITEM игрока не занят.
	if (random() < 0.8)
		sound(self, CHAN_VOICE, "zombie/z_hit.wav", 1, ATTN_NORM);
	else
		sound(self, CHAN_VOICE, "zombie/z_miss.wav", 1, ATTN_NORM);
};
