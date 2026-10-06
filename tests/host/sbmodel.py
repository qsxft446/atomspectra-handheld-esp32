"""Модель #SB-1 (spectrum.c, коммит свипа): база счёта/времени. Счёт — один канал = total."""


class Board:
    def __init__(s):
        s.valid = False; s.shown_c = 0; s.shown_t = 0
        s.base_c = 0; s.base_t = 0; s.guard = 0; s.first = True; s.defers = 0; s.now = 0; s.folds = 0
        s.quiet = 0       # после переноса по счёту столько свипов без проверки по времени

    def reset_user(s):
        s.valid = False; s.shown_c = 0; s.shown_t = 0; s.base_c = 0; s.base_t = 0; s.guard = s.now + 15; s.quiet = 0

    def session(s):
        s.first = True; s.defers = 0

    def commit(s, dev_c, stat_t):   # stat_t=None — STAT не свежий
        s.now += 1
        guard = s.now < s.guard
        if s.valid and not guard:
            reset = dev_c < s.shown_c - s.base_c
            quiet = s.quiet > 0
            if s.quiet > 0: s.quiet -= 1
            if reset:
                s.quiet = 5
            if not reset and stat_t is not None and not quiet:
                el = s.shown_t - s.base_t
                reset = stat_t + 5 < el and stat_t < el // 2
            if not reset and stat_t is None and s.first and s.defers < 3:
                s.defers += 1
                return
            if reset:
                s.base_c = s.shown_c; s.folds += 1
        s.first = False
        s.shown_c = s.base_c + dev_c
        base_t = s.base_t
        dev_prev = s.shown_t - base_t if s.shown_t >= base_t else 0
        expected = dev_prev + 1
        if stat_t is not None:
            t = stat_t
            if s.valid and not guard and t + 5 < dev_prev:
                base_t = s.shown_t + 1 - t if s.shown_t + 1 >= t else 0
                s.base_t = base_t
            elif s.valid and t + 5 >= dev_prev and t < expected:
                t = expected
            s.shown_t = base_t + t
        elif s.valid:
            s.shown_t = base_t + expected
        else:
            s.shown_t = base_t + dev_prev + 1
        s.valid = True


def run(name, steps):
    b = Board(); out = []
    for st in steps:
        if st == "reset": b.reset_user(); continue
        if st == "session": b.session(); continue
        if isinstance(st, tuple) and st[0] == "boot":   # перезагрузка платы: восстановление
            _, c, t, bc, bt = st
            b = Board(); b.valid = True; b.shown_c, b.shown_t, b.base_c, b.base_t = c, t, bc, bt; b.session(); continue
        dc, dt = st
        b.commit(dc, dt)
        out.append((b.shown_c, b.shown_t))
    print(f"{name:44s} folds={b.folds}  {out}")


R = 100   # отсчётов в секунду
run("перезагрузка прибора (время прибора идёт)",
    [(1000*R + i*R, 1000 + i) for i in range(3)] + ["session", (0, 1003), (0, 1004)] + [(i*R, 1004 + i) for i in range(1, 4)])
run("обрыв питания прибора (время в 0)",
    [(1000*R + i*R, 1000 + i) for i in range(3)] + ["session", (0, 0), (R, 1), (2*R, 2)])
run("перезапуск: в первом свипе старый STAT (замер)",
    [(1000*R + i*R, 1000 + i) for i in range(3)] + [(0, 1002), (R, 1), (2*R, 2), (3*R, 3)])
run("перезагрузка только платы",
    [("boot", 1000*R, 1000, 0, 0), (1030*R, 1030), (1031*R, 1031)])
run("плата и прибор вместе",
    [("boot", 1000*R, 1000, 0, 0), (2*R, 2), (3*R, 3)])
run("сброс пользователя",
    [(1000*R, 1000), "reset", (1000*R, 1001), (R, 1), (2*R, 2)])
run("перезапуск, первый свип без STAT (горячий)",
    [(1000*R, 1000), "session", (2000*R, None), (2001*R, 3), (2002*R, 4)])
