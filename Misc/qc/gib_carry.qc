/*
    gib_carry.qc - гибы можно поднимать на +use

    ВАЖНО: это ваши же ThrowGib/ThrowHead, в них добавлено по ОДНОЙ строке -
        new.physdynamic = 1;   (ThrowGib)
        self.physdynamic = 1;  (ThrowHead)
    всё остальное - ваш код.

    ЧТО НУЖНО СДЕЛАТЬ, ЧТОБЫ ЭТО ЗАРАБОТАЛО (три вещи, все обязательные)

    1. Движок с этой правкой (пересобрать quakespasm.sln). В нём переноска
       принимает не только SOLID_BBOX, но и SOLID_TRIGGER - именно триггерами
       и являются гибы, потому что вы сами ставите им `// myach`.

    2. Ниже в defs.qc - четыре объявления (файл Misc/qc/defs_physdynamic.qc,
       его надо взять целиком):
           .float physdynamic;
           .float physcarry;
           .float physmovetype;
           .float physsolid;
       Без них компилятор QuakeC не даст написать self.physdynamic: движок поле
       знает (оно зарегистрировано в pr_edict.c), а компилятор - нет.

    3. Собственно строки physdynamic = 1 в ThrowGib/ThrowHead - по одной на
       функцию.

    ПОЧЕМУ ТЕГ, А НЕ ПРОСТО "всё, что SOLID_TRIGGER"

    Триггеров в игре полно: телепорты, зоны, триггеры карты. Движок берёт в
    руки только то, у чего стоит physdynamic, поэтому ни телепорт, ни
    trigger_multiple поднять нельзя. Гибы тегируем сами, в момент спавна.

    ЧТО ИЗМЕНИЛОСЬ В ПОВЕДЕНИИ ГИБА ПОСЛЕ ЭТОГО

    Ничего, пока его не взяли. Взятый гиб:
      * перестаёт быть триггером (solid = SOLID_NOT), поэтому выходить из рук
        и кувыркаться он не может: avelocity обнуляется, поворот - только по
        рысканию, вслед за обзором;
      * висит в 48 юнитах перед глазами и упирается в стену;
      * при отпускании возвращается ровно туда, чем он был: solid =
        SOLID_TRIGGER, movetype = MOVETYPE_BOUNCE (движок сохраняет и solid, и
        movetype в полях physsolid/physmovetype, это его работа);
      * получает скорость игрока, то есть падает туда, куда вы шли.

    ЧТО НЕ РАБОТАЕТ И ПОЧЕМУ (честно)

    1. Гиб с `setsize ('0 0 0', '0 0 0')` держится в руках за точку. Модель
       гиба больше этой точки, поэтому у стены она может частично "войти" в
       стену: обрезается origin, а габаритов у энтити нет. Лечится реальным
       размером - строки для этого есть в конце файла, но по умолчанию я их НЕ
       включал, потому что это меняет и область, в которой гиб ловит touch
       (kick_touch).

    2. Гиб в руках продолжает жить своей жизнью: `nextthink = time + 10 +
       random()*10` и `SUB_Remove` срабатывают и в руках. То есть через
       10-20 секунд гиб исчезает прямо из рук. Движок это переживает
       (SV_CarryHeld проверяет, что энтити жива и что это правда она), но если
       хотите, чтобы из рук не пропадало - в touch/think надо сдвигать
       nextthink, пока объект в руках. Я этого не делал: это уже ваша
       механика.

    3. Голова (ThrowHead) - это эдикт самого монстра, у него остаётся
       FL_MONSTER. Движок запрещает брать живого монстра, но разрешает мёртвого:
       проверка именно `FL_MONSTER и health > 0`. Поэтому в ThrowHead ниже
       добавлено `self.health = 0;` - чтобы эдикт гарантированно считался
       трупом, а не живым монстром, даже если смерть пришла от чего-то, что
       health не обнулило.

    4. Найти гиб прицелом нельзя: триггеры не участвуют ни в одной
       SV_Move-трассе. Его находит второй проход - "что лежит в пределах 64
       юнитов и впереди". Значит гиб надо искать глазами впереди себя, а не
       наводить на него прицел; если гиб под ногами, шагните назад.
*/

/*
=================
ThrowGib

У вас добавлена ровно одна строка - new.physdynamic = 1;
=================
*/
void(string gibname, float dm) ThrowGib =
{
	local	entity new;

	new = spawn();
	new.origin = self.origin;
	setmodel (new, gibname);
	setsize (new, '0 0 0', '0 0 0');
	new.velocity = VelocityForDamage (dm);
	new.movetype = MOVETYPE_BOUNCE;
	new.solid = SOLID_TRIGGER;   // myach
	new.touch = kick_touch;
	new.avelocity_x = random()*600;
	new.avelocity_y = random()*600;
	new.avelocity_z = random()*600;
	new.think = SUB_Remove;
	new.ltime = time;
	new.nextthink = time + 10 + random()*10;
	new.frame = 0;
	new.flags = 0;

	// единственная добавленная строка: этот гиб можно взять на +use
	new.physdynamic = 1;

	// Если хотите, чтобы гиб не прятался в стене, когда его несёте, -
	// раскомментируйте: у гиба появится настоящий размер. Учтите, что от
	// размера зависит и то, на каком расстоянии гиб ловит touch (kick_touch),
	// то есть пинать его станет проще.
	// setsize (new, '-8 -8 -8', '8 8 8');
};

/*
=================
ThrowHead

У вас добавлено две строки: self.health = 0; и self.physdynamic = 1;
=================
*/
void(string gibname, float dm) ThrowHead =
{
	setmodel (self, gibname);
	self.frame = 0;
	self.nextthink = -1;
	self.movetype = MOVETYPE_BOUNCE;
	self.takedamage = DAMAGE_NO;
	self.solid = SOLID_TRIGGER; //myach
	self.touch = kick_touch;
	self.view_ofs = '0 0 8';
	setsize (self, '-16 -16 0', '16 16 56');
	self.velocity = VelocityForDamage (dm);
	self.origin_z = self.origin_z - 24;
	self.flags = self.flags - (self.flags & FL_ONGROUND);
	self.avelocity = crandom() * '0 600 0';

	// это уже не монстр, а голова: health = 0 делает эдикт трупом, и тогда
	// движок разрешает его взять, несмотря на оставшийся FL_MONSTER
	self.health = 0;
	self.physdynamic = 1;
};
