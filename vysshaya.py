"""Высшая и прикладная математика в стандартной библиотеке языка Дан.

Разделы: элементарные функции, начала анализа (производные и интегралы
многочленов), комбинаторика, теория вероятностей и статистика, линейная
алгебра, аналитическая геометрия, планиметрия и стереометрия, дискретная
математика и системы счисления, числа прописью, прогрессии, финансовая
математика, логика, линейное программирование (симплекс-метод).

Функции регистрируются в общей таблице matematika.ФУНКЦИИ, поэтому
доступны в любой программе без подключения и, как вся библиотека, не
резервируют имён. Код C (КОД_C) вставляется после кода многочленов.
"""

import matematika as м
from matematika import ЦЕЛ, ДРОБ, ЛОГ, РАЦ, СТР, МН

СЦ, СР, СД, ССТ = "[цел]", "[рац]", "[дроб]", "[стр]"
МР, МД = "[[рац]]", "[[дроб]]"
НЕОБЯЗ_СР, НЕОБЯЗ_ЦЕЛ, НЕОБЯЗ_ДРОБ = "[рац]?", "цел?", "дроб?"

# Типы, которые должен зарегистрировать генератор, чтобы код C библиотеки собрался.
НУЖНЫЕ_ТИПЫ = (МД, НЕОБЯЗ_СР, НЕОБЯЗ_ЦЕЛ, НЕОБЯЗ_ДРОБ, "цел!", "дроб!")


def д(имя, варианты, описание):
    м._добавить(имя, варианты, описание)


# ---------- элементарные функции ----------

for имена, c, описание in (
        (("арктангенс", "arctg"), "atan", "арктангенс: угол в радианах от -пи/2 до пи/2"),
        (("экспонента", "exp"), "exp", "е в степени х"),
):
    for имя in имена:
        д(имя, [([ДРОБ], ДРОБ, c, False)], описание)
for имена, c, описание in (
        (("синус", "sin"), "dan_sin", "синус угла в радианах: синус(пи / 6) = 0.5"),
        (("косинус", "cos"), "dan_cos", "косинус угла в радианах"),
        (("тангенс", "tg"), "dan_tan", "тангенс угла в радианах"),
        (("котангенс", "ctg"), "dan_ctg", "котангенс угла в радианах"),
        (("арксинус", "arcsin"), "dan_asin", "арксинус: угол от -пи/2 до пи/2; аргумент от -1 до 1"),
        (("арккосинус", "arccos"), "dan_acos", "арккосинус: угол от 0 до пи; аргумент от -1 до 1"),
        (("арккотангенс", "arcctg"), "dan_acot", "арккотангенс: угол от 0 до пи"),
        (("ln",), "dan_ln", "натуральный логарифм (по основанию е)"),
        (("lg",), "dan_lg", "десятичный логарифм"),
):
    for имя in имена:
        д(имя, [([ДРОБ], ДРОБ, c, True)], описание)
д("логарифм", [([ДРОБ, ДРОБ], ДРОБ, "dan_log", True)],
  "логарифм числа по основанию: логарифм(2, 8) = 3.0 (сначала основание, затем число)")
д("в_радианы", [([ДРОБ], ДРОБ, "dan_to_radians", False)], "градусы в радианы: в_радианы(180) = 3.14159...")
д("в_градусы", [([ДРОБ], ДРОБ, "dan_to_degrees", False)], "радианы в градусы: в_градусы(пи) = 180.0")
м.ФУНКЦИИ["корень"]["варианты"].append(([ДРОБ, ЦЕЛ], ДРОБ))
м.ФУНКЦИИ["корень"]["c"].append("dan_nroot")
м.ФУНКЦИИ["корень"]["строка"].append(True)
д("знак", [([ЦЕЛ], ЦЕЛ, "dan_sign_int", False), ([РАЦ], ЦЕЛ, "dan_sign_rat", False),
           ([ДРОБ], ЦЕЛ, "dan_sign_float", False)], "знак числа: 1, 0 или -1")
д("округлить_вниз", [([РАЦ], ЦЕЛ, "dan_floor_rat", False), ([ДРОБ], ЦЕЛ, "dan_floor_float", True)],
  "наибольшее целое, не превосходящее число (целая часть): округлить_вниз(-2.5) = -3")
д("целая_часть", [([РАЦ], ЦЕЛ, "dan_floor_rat", False), ([ДРОБ], ЦЕЛ, "dan_floor_float", True)],
  "целая часть числа [х]: наибольшее целое, не превосходящее х")
д("округлить_вверх", [([РАЦ], ЦЕЛ, "dan_ceil_rat", False), ([ДРОБ], ЦЕЛ, "dan_ceil_float", True)],
  "наименьшее целое, не меньшее числа: округлить_вверх(2.1) = 3")
д("дробная_часть", [([РАЦ], РАЦ, "dan_frac_rat", True), ([ДРОБ], ДРОБ, "dan_frac_float", False)],
  "дробная часть {х} = х - [х]: дробная_часть(рац(7, 2)) = 1/2")
м.КОНСТАНТЫ["е"] = (ДРОБ, "2.718281828459045")
м.КОНСТАНТЫ["e"] = (ДРОБ, "2.718281828459045")

# ---------- начала анализа: многочлены ----------

д("производная", [([МН], МН, "dan_poly_derivative", True), ([МН, ЦЕЛ], МН, "dan_poly_derivative_n", True)],
  "производная многочлена (вторым значением - порядок): производная(х³) = 3х²")
д("первообразная", [([МН], МН, "dan_poly_antiderivative", True)],
  "первообразная многочлена с постоянной 0: первообразная(3х²) = х³")
д("интеграл", [([МН, РАЦ, РАЦ], РАЦ, "dan_poly_integral_rat", True),
               ([МН, ДРОБ, ДРОБ], ДРОБ, "dan_poly_integral_float", True)],
  "определённый интеграл многочлена от а до б (формула Ньютона - Лейбница): интеграл(х², 0, 3) = 9")
д("касательная", [([МН, РАЦ], МН, "dan_poly_tangent", True)],
  "уравнение касательной к графику многочлена в точке: касательная(х², 1) = 2х - 1")
д("критические_точки", [([МН], СД, "dan_poly_critical", True)],
  "точки, где производная равна нулю (возможные экстремумы): критические_точки(х³ - 3х) = [-1.0, 1.0]")

# ---------- комбинаторика ----------

д("факториал", [([ЦЕЛ], ЦЕЛ, "dan_factorial", True)], "факториал: факториал(5) = 120")
д("перестановки", [([ЦЕЛ], ЦЕЛ, "dan_factorial", True)], "число перестановок из н элементов: н!")
д("размещения", [([ЦЕЛ, ЦЕЛ], ЦЕЛ, "dan_arrangements", True)],
  "число размещений из н по к (важен порядок): размещения(5, 2) = 20")
д("сочетания", [([ЦЕЛ, ЦЕЛ], ЦЕЛ, "dan_binom", True)],
  "число сочетаний из н по к (порядок не важен): сочетания(5, 2) = 10")
д("размещения_с_повторениями", [([ЦЕЛ, ЦЕЛ], ЦЕЛ, "dan_arr_rep", True)], "н в степени к")
д("сочетания_с_повторениями", [([ЦЕЛ, ЦЕЛ], ЦЕЛ, "dan_comb_rep", True)],
  "число сочетаний с повторениями из н по к")
д("перестановки_с_повторениями", [([СЦ], ЦЕЛ, "dan_multinomial", True)],
  "число перестановок с повторениями по числу повторений каждого элемента: "
  "перестановки_с_повторениями([2, 1]) = 3")

# ---------- вероятность и статистика ----------

д("вероятность", [([ЦЕЛ, ЦЕЛ], РАЦ, "dan_classical_prob", True)],
  "классическая вероятность: благоприятные исходы / все исходы: вероятность(1, 6) = 1/6")
д("бернулли", [([ЦЕЛ, ЦЕЛ, РАЦ], РАЦ, "dan_bernoulli_rat", True),
               ([ЦЕЛ, ЦЕЛ, ДРОБ], ДРОБ, "dan_bernoulli_float", True)],
  "вероятность ровно к успехов в н испытаниях с вероятностью успеха п (формула Бернулли)")
д("математическое_ожидание", [([СР, СР], РАЦ, "dan_expect_rat", True), ([СД, СД], ДРОБ, "dan_expect_float", True)],
  "математическое ожидание: значения и их вероятности")
for имя, c_р, c_д, описание in (
        ("дисперсия", "dan_var_rat", "dan_var_float", "дисперсия набора чисел (средний квадрат отклонения от среднего)"),
        ("дисперсия_выборки", "dan_svar_rat", "dan_svar_float", "исправленная дисперсия выборки (делитель н - 1)"),
):
    д(имя, [([СЦ], РАЦ, c_р + "_i", True), ([СР], РАЦ, c_р, True), ([СД], ДРОБ, c_д, True)], описание)
м.ФУНКЦИИ["дисперсия"]["варианты"].append(([СР, СР], РАЦ))
м.ФУНКЦИИ["дисперсия"]["c"].append("dan_var_dist_rat")
м.ФУНКЦИИ["дисперсия"]["строка"].append(True)
д("стандартное_отклонение", [([СЦ], ДРОБ, "dan_sd_i", True), ([СР], ДРОБ, "dan_sd_rat", True),
                             ([СД], ДРОБ, "dan_sd_float", True)], "среднее квадратическое отклонение")
д("корреляция", [([СЦ, СЦ], ДРОБ, "dan_corr_i", True), ([СД, СД], ДРОБ, "dan_corr_float", True)],
  "коэффициент корреляции Пирсона двух рядов: от -1 до 1")
д("линейная_регрессия", [([СЦ, СЦ], СД, "dan_linreg_i", True), ([СД, СД], СД, "dan_linreg_float", True)],
  "прямая у = к х + б, наилучшая по методу наименьших квадратов: результат [к, б]")
д("энтропия", [([СД], ДРОБ, "dan_entropy", True)],
  "информационная энтропия распределения в битах: энтропия([0.5, 0.5]) = 1.0")
д("случайное", [([ЦЕЛ, ЦЕЛ], ЦЕЛ, "dan_random_int", True), ([], ДРОБ, "dan_random_float", False)],
  "случайное целое от а до б включительно; без аргументов - случайное дробное от 0 до 1")
д("перемешать", [([СЦ], СЦ, "dan_shuffle_i", False), ([ССТ], ССТ, "dan_shuffle_s", False),
                 ([СР], СР, "dan_shuffle_r", False), ([СД], СД, "dan_shuffle_f", False)],
  "новый список с теми же элементами в случайном порядке")

# ---------- линейная алгебра ----------

д("определитель", [([МР], РАЦ, "dan_det_rat", True), ([МД], ДРОБ, "dan_det_float", True)],
  "определитель квадратной матрицы: определитель([[1, 2], [3, 4]]) = -2")
д("обратная_матрица", [([МР], МР, "dan_inverse_rat", True), ([МД], МД, "dan_inverse_float", True)],
  "обратная матрица (точно для рац)")
д("транспонировать", [([МР], МР, "dan_transpose_rat", True), ([МД], МД, "dan_transpose_float", True)],
  "транспонированная матрица: строки становятся столбцами")
д("умножить_матрицы", [([МР, МР], МР, "dan_matmul_rat", True), ([МД, МД], МД, "dan_matmul_float", True)],
  "произведение матриц")
д("сложить_матрицы", [([МР, МР], МР, "dan_matadd_rat", True), ([МД, МД], МД, "dan_matadd_float", True)],
  "сумма матриц одинакового размера")
д("ранг", [([МР], ЦЕЛ, "dan_rank_rat", True)], "ранг матрицы")
д("единичная_матрица", [([ЦЕЛ], МР, "dan_identity", True)], "единичная матрица порядка н")
д("скалярное_произведение", [([СР, СР], РАЦ, "dan_dot_rat", True), ([СД, СД], ДРОБ, "dan_dot_float", True)],
  "скалярное произведение векторов")
д("векторное_произведение", [([СР, СР], СР, "dan_cross_rat", True), ([СД, СД], СД, "dan_cross_float", True)],
  "векторное произведение трёхмерных векторов")
д("длина_вектора", [([СР], ДРОБ, "dan_norm_rat", False), ([СД], ДРОБ, "dan_norm_float", False)],
  "длина вектора: длина_вектора([3, 4]) = 5.0")
д("угол_между_векторами", [([СД, СД], ДРОБ, "dan_angle_float", True)],
  "угол между векторами в радианах")

# ---------- аналитическая геометрия ----------

for имя, c, результат, описание in (
        ("расстояние", "dan_dist", ДРОБ, "расстояние между точками (х1, у1) и (х2, у2)"),
        ("середина_отрезка", "dan_midpoint", None, "координаты середины отрезка: [х, у]"),
        ("прямая_через_точки", "dan_line_coeffs", None,
         "коэффициенты [а, б, в] уравнения прямой а·х + б·у + в = 0 через две точки"),
        ("уравнение_прямой", "dan_line_str", СТР, "уравнение прямой через две точки: «у = 2х + 1» или «х = 3»"),
):
    if результат is None:
        д(имя, [([РАЦ] * 4, СР, c + "_rat", True), ([ДРОБ] * 4, СД, c + "_float", True)], описание)
    elif результат == ДРОБ:
        д(имя, [([РАЦ] * 4, ДРОБ, c + "_rat", True), ([ДРОБ] * 4, ДРОБ, c + "_float", True)], описание)
    else:
        д(имя, [([РАЦ] * 4, СТР, c, True)], описание)
д("пересечение_прямых", [([РАЦ] * 6, НЕОБЯЗ_СР, "dan_line_intersection", True)],
  "точка пересечения прямых а1·х + б1·у + в1 = 0 и а2·х + б2·у + в2 = 0; "
  "ничего, если прямые параллельны или совпадают")
д("площадь_по_вершинам", [([РАЦ] * 6, РАЦ, "dan_tri_area_rat", True), ([ДРОБ] * 6, ДРОБ, "dan_tri_area_float", False)],
  "площадь треугольника по координатам вершин")
д("площадь_многоугольника", [([СР, СР], РАЦ, "dan_polygon_area", True)],
  "площадь многоугольника по спискам координат вершин (по порядку обхода)")

# ---------- планиметрия и стереометрия ----------

д("гипотенуза", [([ДРОБ, ДРОБ], ДРОБ, "dan_hypot", True)], "гипотенуза по двум катетам (теорема Пифагора)")
д("катет", [([ДРОБ, ДРОБ], ДРОБ, "dan_leg", True)], "катет по гипотенузе и другому катету")
д("площадь_треугольника_по_сторонам", [([ДРОБ, ДРОБ, ДРОБ], ДРОБ, "dan_heron", True)],
  "площадь треугольника по трём сторонам (формула Герона)")
for имя, параметры, c, описание in (
        ("площадь_трапеции", 3, "dan_trapezoid", "площадь трапеции по основаниям и высоте"),
        ("площадь_параллелограмма", 2, "dan_parallelogram", "площадь параллелограмма по стороне и высоте"),
        ("площадь_ромба", 2, "dan_rhombus", "площадь ромба по диагоналям"),
        ("объём_пирамиды", 2, "dan_pyramid", "объём пирамиды по площади основания и высоте"),
        ("объём_призмы", 2, "dan_prism", "объём призмы по площади основания и высоте"),
        ("площадь_поверхности_куба", 1, "dan_cube_surface", "площадь поверхности куба по ребру"),
):
    д(имя, [([РАЦ] * параметры, РАЦ, c + "_rat", True), ([ДРОБ] * параметры, ДРОБ, c + "_float", False)], описание)
for имя, параметры, c, описание in (
        ("объём_шара", 1, "dan_ball_volume", "объём шара по радиусу"),
        ("площадь_сферы", 1, "dan_sphere_area", "площадь сферы по радиусу"),
        ("объём_цилиндра", 2, "dan_cyl_volume", "объём цилиндра по радиусу и высоте"),
        ("площадь_поверхности_цилиндра", 2, "dan_cyl_area", "полная площадь поверхности цилиндра"),
        ("объём_конуса", 2, "dan_cone_volume", "объём конуса по радиусу и высоте"),
        ("длина_дуги", 2, "dan_arc_length", "длина дуги окружности по радиусу и углу в градусах"),
        ("площадь_сектора", 2, "dan_sector_area", "площадь кругового сектора по радиусу и углу в градусах"),
):
    д(имя, [([ДРОБ] * параметры, ДРОБ, c, False)], описание)
д("сумма_углов_многоугольника", [([ЦЕЛ], ЦЕЛ, "dan_polygon_angles", True)],
  "сумма внутренних углов выпуклого н-угольника в градусах")

# ---------- дискретная математика, системы счисления, прописью ----------

д("в_систему", [([ЦЕЛ, ЦЕЛ], СТР, "dan_to_base", True)],
  "запись числа в системе счисления с основанием от 2 до 36: в_систему(10, 2) = \"1010\"")
д("из_системы", [([СТР, ЦЕЛ], ЦЕЛ, "dan_from_base", True)],
  "число по записи в системе счисления: из_системы(\"FF\", 16) = 255")
д("римское", [([ЦЕЛ], СТР, "dan_to_roman", True)], "запись римскими цифрами (1-3999): римское(2026) = \"MMXXVI\"")
д("из_римского", [([СТР], ЦЕЛ, "dan_from_roman", True)], "число по римской записи")
д("степень_по_модулю", [([ЦЕЛ, ЦЕЛ, ЦЕЛ], ЦЕЛ, "dan_powmod", True)],
  "остаток от деления а в степени н на м (без переполнения)")
д("обратный_по_модулю", [([ЦЕЛ, ЦЕЛ], НЕОБЯЗ_ЦЕЛ, "dan_invmod", True)],
  "число х, для которого а·х даёт остаток 1 при делении на м; ничего, если такого нет")
д("фибоначчи", [([ЦЕЛ], ЦЕЛ, "dan_fibonacci", True)], "н-е число Фибоначчи: фибоначчи(10) = 55")
д("прописью", [([ЦЕЛ], СТР, "dan_in_words", False)],
  "число словами: прописью(1251) = \"одна тысяча двести пятьдесят один\"")
д("сумма_прописью", [([РАЦ], СТР, "dan_money_words_rat", True), ([ДРОБ], СТР, "dan_money_words_float", True)],
  "денежная сумма словами: \"сто двадцать тысяч рублей 50 копеек\"")
д("сумма_цифрами", [([РАЦ], СТР, "dan_money_digits_rat", True), ([ДРОБ], СТР, "dan_money_digits_float", True)],
  "денежная сумма цифрами по правилам русского письма: сумма_цифрами(рац(15750025, 100)) = \"157 500,25\"")
д("склонение", [([ЦЕЛ, СТР, СТР, СТР], СТР, "dan_plural", False)],
  "форма слова при числе: склонение(21, \"день\", \"дня\", \"дней\") = \"день\"")

# ---------- прогрессии ----------

for имя, c, описание in (
        ("член_арифметической", "dan_arith_term", "н-й член арифметической прогрессии по первому члену и разности"),
        ("сумма_арифметической", "dan_arith_sum", "сумма первых н членов арифметической прогрессии"),
        ("член_геометрической", "dan_geom_term", "н-й член геометрической прогрессии по первому члену и знаменателю"),
        ("сумма_геометрической", "dan_geom_sum", "сумма первых н членов геометрической прогрессии"),
):
    д(имя, [([РАЦ, РАЦ, ЦЕЛ], РАЦ, c + "_rat", True), ([ДРОБ, ДРОБ, ЦЕЛ], ДРОБ, c + "_float", True)], описание)

# ---------- финансовая математика ----------

д("простые_проценты", [([РАЦ, РАЦ, РАЦ], РАЦ, "dan_simple_interest_rat", True),
                       ([ДРОБ, ДРОБ, ДРОБ], ДРОБ, "dan_simple_interest_float", False)],
  "сумма с простыми процентами: сумма, ставка в процентах годовых, срок в годах")
д("сложные_проценты", [([РАЦ, РАЦ, ЦЕЛ], РАЦ, "dan_compound_rat", True),
                       ([ДРОБ, ДРОБ, ДРОБ], ДРОБ, "dan_compound_float", False),
                       ([ДРОБ, ДРОБ, ДРОБ, ЦЕЛ], ДРОБ, "dan_compound_n", True)],
  "сумма со сложными процентами: сумма, ставка в процентах годовых, срок в годах "
  "(четвёртым значением - число начислений в год)")
д("аннуитетный_платёж", [([ДРОБ, ДРОБ, ЦЕЛ], ДРОБ, "dan_annuity", True)],
  "ежемесячный платёж по кредиту равными частями: сумма, ставка в процентах годовых, число месяцев")
д("приведённая_стоимость", [([ДРОБ, ДРОБ, ДРОБ], ДРОБ, "dan_present_value", False)],
  "сегодняшняя стоимость будущей суммы при ставке дисконтирования (процентов годовых) через срок в годах")
д("чистая_приведённая_стоимость", [([ДРОБ, СД], ДРОБ, "dan_npv", False)],
  "NPV: ставка в процентах и денежные потоки по годам, начиная с нулевого")
д("внутренняя_норма_доходности", [([СД], НЕОБЯЗ_ДРОБ, "dan_irr", True)],
  "IRR в процентах: ставка, при которой NPV потоков равна нулю; ничего, если её нет")
д("реальная_ставка", [([ДРОБ, ДРОБ], ДРОБ, "dan_real_rate", False)],
  "реальная ставка в процентах по номинальной ставке и инфляции (формула Фишера)")
д("проценты_за_дни", [([РАЦ, РАЦ, ЦЕЛ, ЦЕЛ], РАЦ, "dan_daily_interest", True)],
  "проценты за дни: сумма × ставка / 100 × дней / дней_в_году (например, по ст. 395 ГК РФ)")
д("компенсация_за_задержку", [([РАЦ, РАЦ, ЦЕЛ], РАЦ, "dan_late_pay_comp", True)],
  "компенсация за задержку выплат работнику по ст. 236 ТК РФ: сумма × ключевая ставка / 100 / 150 × дней")

# ---------- разбор чисел с ошибкой как значением ----------

д("разобрать_цел", [([СТР], "цел!", "dan_parse_int_res", True)],
  "целое число из строки или ошибка, если строка - не число: разобрать_цел(\"42\") = 42")
д("разобрать_дроб", [([СТР], "дроб!", "dan_parse_float_res", True)],
  "дробное число из строки (точка или запятая) или ошибка")

# ---------- логика ----------

д("импликация", [([ЛОГ, ЛОГ], ЛОГ, "dan_implies", False)], "«если а, то б»: ложь только при а = истина, б = ложь")
д("эквивалентность", [([ЛОГ, ЛОГ], ЛОГ, "dan_equiv", False)], "истина, если значения одинаковы")
д("исключающее_или", [([ЛОГ, ЛОГ], ЛОГ, "dan_xor", False)], "истина, если ровно одно значение истинно")

# ---------- линейное программирование ----------

д("максимизировать", [([СР, МР, СР], НЕОБЯЗ_СР, "dan_lp_max", True)],
  "наибольшее значение ц·х при ограничениях А·х ≤ б, х ≥ 0 (симплекс-метод, точно); "
  "результат - значения х или ничего, если целевая функция не ограничена")
д("минимизировать", [([СР, МР, СР], НЕОБЯЗ_СР, "dan_lp_min", True)],
  "наименьшее значение ц·х при ограничениях А·х ≤ б, х ≥ 0")

# «е» и «ё» в названиях равнозначны.
м._БЕЗ_Ё.update({имя.replace("ё", "е"): имя for имя in м.ФУНКЦИИ})

м.СООБЩЕНИЯ.update({
    "DAN_TXT_NOT_INT": "\" - не целое число",
    "DAN_TXT_INT_BIG": "\" - слишком большое число для типа цел",
    "DAN_TXT_NOT_FLOAT": "\" - не число",
    "DAN_MSG_TAN": "тангенс не определён для угла пи/2 + пи·к (90° + 180°·к)",
    "DAN_MSG_CTG": "котангенс не определён для угла пи·к (180°·к)",
    "DAN_MSG_ASIN": "арксинус и арккосинус определены для чисел от -1 до 1",
    "DAN_MSG_LOG": "логарифм определён только для положительных чисел",
    "DAN_MSG_LOG_BASE": "основание логарифма должно быть положительным и не равным 1",
    "DAN_MSG_ROOT": "корень чётной степени из отрицательного числа не существует среди действительных чисел",
    "DAN_MSG_ROOT_DEG": "степень корня должна быть натуральным числом",
    "DAN_MSG_NEG_ORDER": "порядок производной не может быть отрицательным",
    "DAN_MSG_NEG_FACT": "факториал определён для целых чисел от 0",
    "DAN_MSG_COMB_RANGE": "должно выполняться 0 ≤ к ≤ н",
    "DAN_MSG_COMB_N": "число элементов н должно быть натуральным",
    "DAN_MSG_PROB_RANGE": "число благоприятных исходов должно быть от 0 до числа всех исходов, а всех исходов - больше 0",
    "DAN_MSG_P_RANGE": "вероятность должна быть от 0 до 1",
    "DAN_MSG_DIST_LEN": "у значений и вероятностей должно быть одинаковое число элементов",
    "DAN_MSG_DIST_SUM": "сумма вероятностей должна быть равна 1",
    "DAN_MSG_STAT_EMPTY": "для расчёта нужен хотя бы один элемент",
    "DAN_MSG_STAT_TWO": "для расчёта нужно хотя бы два элемента",
    "DAN_MSG_SERIES_LEN": "ряды должны иметь одинаковое число элементов",
    "DAN_MSG_CONST_SERIES": "у ряда без разброса значений корреляция и регрессия не определены",
    "DAN_MSG_RANDOM_RANGE": "нижняя граница случайного числа больше верхней",
    "DAN_MSG_MATRIX_EMPTY": "матрица должна содержать хотя бы один элемент",
    "DAN_MSG_MATRIX_RECT": "все строки матрицы должны иметь одинаковую длину",
    "DAN_MSG_MATRIX_SQUARE": "матрица должна быть квадратной",
    "DAN_MSG_MATRIX_SINGULAR": "матрица вырожденная (определитель равен 0), обратной матрицы нет",
    "DAN_MSG_MATRIX_DIMS": "размеры матриц не подходят для этого действия",
    "DAN_MSG_VECTOR_LEN": "векторы должны иметь одинаковую длину",
    "DAN_MSG_VECTOR_3": "векторное произведение определено для трёхмерных векторов",
    "DAN_MSG_ZERO_VECTOR": "угол с нулевым вектором не определён",
    "DAN_MSG_SAME_POINTS": "через совпадающие точки нельзя провести единственную прямую",
    "DAN_MSG_POLYGON": "у многоугольника должно быть хотя бы три вершины и одинаковое число координат х и у",
    "DAN_MSG_TRIANGLE": "из таких отрезков треугольник не составить",
    "DAN_MSG_LEG": "гипотенуза должна быть больше катета, а стороны - положительными",
    "DAN_MSG_NEG_LENGTH": "длина не может быть отрицательной",
    "DAN_MSG_POLYGON_N": "у многоугольника не меньше трёх сторон",
    "DAN_MSG_BASE": "основание системы счисления должно быть от 2 до 36",
    "DAN_MSG_BAD_DIGIT": "в записи числа есть недопустимая для этой системы счисления цифра",
    "DAN_MSG_ROMAN_RANGE": "римскими цифрами записываются числа от 1 до 3999",
    "DAN_MSG_BAD_ROMAN": "неверная запись римскими цифрами",
    "DAN_MSG_MOD": "модуль должен быть положительным, а показатель - неотрицательным",
    "DAN_MSG_FIB": "номер числа Фибоначчи должен быть от 0 до 92",
    "DAN_MSG_NEG_MONEY": "сумма не может быть отрицательной",
    "DAN_MSG_TERM_N": "номер члена прогрессии должен быть натуральным",
    "DAN_MSG_MONTHS": "срок в месяцах должен быть положительным",
    "DAN_MSG_YEAR_DAYS": "число дней в году должно быть положительным, а число дней - неотрицательным",
    "DAN_MSG_LP_DIMS": "размеры цели, матрицы ограничений и правых частей не согласованы",
    "DAN_MSG_LP_RHS": "правые части ограничений должны быть неотрицательными (тогда х = 0 - допустимое решение)",
})

КОД_C = r"""
/* ==== higher and applied mathematics ==== */

/* -- elementary functions -- */
/* Табличные углы (кратные пи/12, то есть 15°) дают точные табличные значения:
   синус(пи / 6) = 0.5, а не 0.49999999999999994, как в других языках. */
static bool dan_table_angle(double x, int64_t *k) {
    double q = x / (3.141592653589793 / 12);
    if (!(fabs(q) < 1e12)) return false;
    double r = nearbyint(q);
    if (fabs(q - r) > 1e-9) return false;
    *k = (((int64_t)r % 24) + 24) % 24;
    return true;
}
static double dan_sin_table(int64_t k, double x) {   /* sin(k·15°), k = 0..23 */
    static const double h = 0.7071067811865476, t = 0.8660254037844386;
    switch (k) {
        case 0: case 12: return 0.0;
        case 2: case 10: return 0.5;
        case 3: case 9: return h;
        case 4: case 8: return t;
        case 6: return 1.0;
        case 14: case 22: return -0.5;
        case 15: case 21: return -h;
        case 16: case 20: return -t;
        case 18: return -1.0;
    }
    return sin(x);
}
static double dan_sin(double x, int line) {
    (void)line;
    int64_t k;
    return dan_table_angle(x, &k) ? dan_sin_table(k, x) : sin(x);
}
static double dan_cos(double x, int line) {
    (void)line;
    int64_t k;
    return dan_table_angle(x, &k) ? dan_sin_table((k + 6) % 24, x + 1.5707963267948966) : cos(x);
}
static double dan_tan(double x, int line) {
    int64_t k;
    if (dan_table_angle(x, &k)) {
        double s = dan_sin_table(k, x), c = dan_sin_table((k + 6) % 24, x + 1.5707963267948966);
        if (c == 0) dan_error(DAN_MSG_TAN, line);
        switch (k % 12) {
            case 0: return 0.0;
            case 2: return 0.5773502691896257;     /* √3/3, ближайшее двоичное число */
            case 3: return 1.0;
            case 4: return 1.7320508075688772;     /* √3 */
            case 8: return -1.7320508075688772;
            case 9: return -1.0;
            case 10: return -0.5773502691896257;
        }
        return s / c;
    }
    return tan(x);
}
static double dan_ctg(double x, int line) {
    int64_t k;
    if (dan_table_angle(x, &k)) {
        double s = dan_sin_table(k, x), c = dan_sin_table((k + 6) % 24, x + 1.5707963267948966);
        if (s == 0) dan_error(DAN_MSG_CTG, line);
        switch (k % 12) {
            case 6: return 0.0;
            case 2: return 1.7320508075688772;
            case 3: return 1.0;
            case 4: return 0.5773502691896257;
            case 8: return -0.5773502691896257;
            case 9: return -1.0;
            case 10: return -1.7320508075688772;
        }
        return c / s;
    }
    return cos(x) / sin(x);
}
static double dan_asin(double x, int line) { if (x < -1 || x > 1) dan_error(DAN_MSG_ASIN, line); return asin(x); }
static double dan_acos(double x, int line) { if (x < -1 || x > 1) dan_error(DAN_MSG_ASIN, line); return acos(x); }
static double dan_acot(double x, int line) { (void)line; return 1.5707963267948966 - atan(x); }
static double dan_ln(double x, int line) { if (!(x > 0)) dan_error(DAN_MSG_LOG, line); return log(x); }
static double dan_lg(double x, int line) { if (!(x > 0)) dan_error(DAN_MSG_LOG, line); return log10(x); }
static double dan_log(double b, double x, int line) {
    if (!(b > 0) || b == 1) dan_error(DAN_MSG_LOG_BASE, line);
    if (!(x > 0)) dan_error(DAN_MSG_LOG, line);
    double r = log(x) / log(b);
    double k = nearbyint(r);
    if (fabs(r - k) < 1e-12 && pow(b, k) == x) return k;   /* логарифм(2, 8) = 3.0 ровно */
    return r;
}
static double dan_to_radians(double g) { return g * 3.141592653589793 / 180.0; }
static double dan_to_degrees(double r) { return r * 180.0 / 3.141592653589793; }
static double dan_nroot(double x, int64_t n, int line) {
    if (n < 1) dan_error(DAN_MSG_ROOT_DEG, line);
    if (n == 1) return x;
    if (n == 2) { if (x < 0) dan_error(DAN_MSG_ROOT, line); return sqrt(x); }
    if (n == 3) return cbrt(x);
    if (x < 0) { if (n % 2 == 0) dan_error(DAN_MSG_ROOT, line); return -pow(-x, 1.0 / (double)n); }
    double r = pow(x, 1.0 / (double)n), k = nearbyint(r);
    if (fabs(r - k) < 1e-9 && pow(k, (double)n) == x) return k;
    return r;
}
static int64_t dan_sign_int(int64_t x) { return (x > 0) - (x < 0); }
static int64_t dan_sign_rat(dan_rat x) { return (x.n > 0) - (x.n < 0); }
static int64_t dan_sign_float(double x) { return (x > 0) - (x < 0); }
static int64_t dan_floor_rat(dan_rat x) { int64_t q = x.n / x.d; if (x.n % x.d != 0 && x.n < 0) q--; return q; }
static int64_t dan_ceil_rat(dan_rat x) { int64_t q = x.n / x.d; if (x.n % x.d != 0 && x.n > 0) q++; return q; }
static int64_t dan_float_to_i64(double v, int line) {
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) dan_error(DAN_MSG_FLOAT_RANGE, line);
    return (int64_t)v;
}
static int64_t dan_floor_float(double x, int line) { return dan_float_to_i64(floor(x), line); }
static int64_t dan_ceil_float(double x, int line) { return dan_float_to_i64(ceil(x), line); }
static dan_rat dan_frac_rat(dan_rat x, int line) { return dan_rat_sub(x, dan_rat_from_int(dan_floor_rat(x), line), line); }
static double dan_frac_float(double x) { return x - floor(x); }

/* -- calculus of polynomials -- */
static dan_mn dan_poly_derivative_n(dan_mn p, int64_t n, int line) {
    if (n < 0) dan_error(DAN_MSG_NEG_ORDER, line);
    for (int64_t i = 0; i < n && p->deg >= 0; i++) p = dan_poly_derivative(p, line);
    return p;
}
static dan_mn dan_poly_antiderivative(dan_mn p, int line) {
    dan_poly *r = dan_poly_alloc(p->deg + 1);
    for (int64_t i = 0; i <= p->deg; i++) r->c[i + 1] = dan_rat_div(p->c[i], dan_rat_from_int(i + 1, line), line);
    return dan_poly_trim(r);
}
static dan_rat dan_poly_integral_rat(dan_mn p, dan_rat a, dan_rat b, int line) {
    dan_mn f = dan_poly_antiderivative(p, line);
    return dan_rat_sub(dan_poly_eval_rat(f, b, line), dan_poly_eval_rat(f, a, line), line);
}
static double dan_poly_integral_float(dan_mn p, double a, double b, int line) {
    dan_mn f = dan_poly_antiderivative(p, line);
    return dan_poly_eval_float(f, b) - dan_poly_eval_float(f, a);
}
static dan_mn dan_poly_tangent(dan_mn p, dan_rat a, int line) {
    dan_rat fa = dan_poly_eval_rat(p, a, line);
    dan_rat k = dan_poly_eval_rat(dan_poly_derivative(p, line), a, line);
    dan_mn kx = dan_poly_scale(dan_poly_x(), k, line);
    return dan_poly_add(kx, dan_poly_const(dan_rat_sub(fa, dan_rat_mul(k, a, line), line)), line);
}
static dl_Lf *dan_poly_critical(dan_mn p, int line) {
    dan_mn d = dan_poly_derivative(p, line);
    if (d->deg < 1) return dl_Lf_from(0, NULL);
    return dan_solve_real(d, line);
}

/* -- combinatorics -- */
static int64_t dan_mul_chk(int64_t a, int64_t b, int line) {
    int64_t r;
    if (__builtin_mul_overflow(a, b, &r)) dan_error(DAN_MSG_TOO_BIG, line);
    return r;
}
static int64_t dan_factorial(int64_t n, int line) {
    if (n < 0) dan_error(DAN_MSG_NEG_FACT, line);
    int64_t r = 1;
    for (int64_t i = 2; i <= n; i++) r = dan_mul_chk(r, i, line);
    return r;
}
static void dan_comb_check(int64_t n, int64_t k, int line) { if (k < 0 || n < 0 || k > n) dan_error(DAN_MSG_COMB_RANGE, line); }
static int64_t dan_arrangements(int64_t n, int64_t k, int line) {
    dan_comb_check(n, k, line);
    int64_t r = 1;
    for (int64_t i = 0; i < k; i++) r = dan_mul_chk(r, n - i, line);
    return r;
}
static int64_t dan_binom(int64_t n, int64_t k, int line) {
    dan_comb_check(n, k, line);
    if (k > n - k) k = n - k;
    dan_u128 r = 1;
    for (int64_t i = 1; i <= k; i++) {
        r = r * (dan_u128)(n - k + i) / (dan_u128)i;     /* делится нацело на каждом шаге */
        if (r > (dan_u128)INT64_MAX) dan_error(DAN_MSG_TOO_BIG, line);
    }
    return (int64_t)r;
}
static int64_t dan_arr_rep(int64_t n, int64_t k, int line) {
    if (n < 0 || k < 0) dan_error(DAN_MSG_COMB_RANGE, line);
    return dan_pow_int(n, k, line);
}
static int64_t dan_comb_rep(int64_t n, int64_t k, int line) {
    if (n < 1) dan_error(DAN_MSG_COMB_N, line);
    if (k < 0) dan_error(DAN_MSG_COMB_RANGE, line);
    int64_t t;
    if (__builtin_add_overflow(n, k - 1, &t)) dan_error(DAN_MSG_TOO_BIG, line);
    return dan_binom(t, k, line);
}
static int64_t dan_multinomial(const dl_Li *l, int line) {
    int64_t total = 0, r = 1;
    for (int64_t i = 0; i < l->len; i++) {
        if (l->items[i] < 0) dan_error(DAN_MSG_COMB_RANGE, line);
        if (__builtin_add_overflow(total, l->items[i], &total)) dan_error(DAN_MSG_TOO_BIG, line);
        r = dan_mul_chk(r, dan_binom(total, l->items[i], line), line);
    }
    return r;
}

/* -- probability and statistics -- */
static dan_rat dan_classical_prob(int64_t m, int64_t n, int line) {
    if (n <= 0 || m < 0 || m > n) dan_error(DAN_MSG_PROB_RANGE, line);
    return dan_rat_of(m, n, line);
}
static dan_rat dan_bernoulli_rat(int64_t n, int64_t k, dan_rat p, int line) {
    if (p.n < 0 || p.n > p.d) dan_error(DAN_MSG_P_RANGE, line);
    dan_rat c = dan_rat_from_int(dan_binom(n, k, line), line);
    dan_rat q = dan_rat_sub(dan_rat_from_int(1, line), p, line);
    return dan_rat_mul(c, dan_rat_mul(dan_rat_pow(p, k, line), dan_rat_pow(q, n - k, line), line), line);
}
static double dan_bernoulli_float(int64_t n, int64_t k, double p, int line) {
    if (!(p >= 0 && p <= 1)) dan_error(DAN_MSG_P_RANGE, line);
    dan_comb_check(n, k, line);
    double lc = lgamma((double)n + 1) - lgamma((double)k + 1) - lgamma((double)(n - k) + 1);
    if (p == 0) return k == 0 ? 1.0 : 0.0;
    if (p == 1) return k == n ? 1.0 : 0.0;
    return exp(lc + (double)k * log(p) + (double)(n - k) * log1p(-p));
}
static void dan_dist_check_rat(const dl_Lr *x, const dl_Lr *p, int line) {
    if (x->len != p->len) dan_error(DAN_MSG_DIST_LEN, line);
    if (x->len == 0) dan_error(DAN_MSG_STAT_EMPTY, line);
    dan_rat s = { 0, 1 };
    for (int64_t i = 0; i < p->len; i++) {
        if (p->items[i].n < 0) dan_error(DAN_MSG_P_RANGE, line);
        s = dan_rat_add(s, p->items[i], line);
    }
    if (!(s.n == 1 && s.d == 1)) dan_error(DAN_MSG_DIST_SUM, line);
}
static dan_rat dan_expect_rat(const dl_Lr *x, const dl_Lr *p, int line) {
    dan_dist_check_rat(x, p, line);
    dan_rat s = { 0, 1 };
    for (int64_t i = 0; i < x->len; i++) s = dan_rat_add(s, dan_rat_mul(x->items[i], p->items[i], line), line);
    return s;
}
static double dan_expect_float(const dl_Lf *x, const dl_Lf *p, int line) {
    if (x->len != p->len) dan_error(DAN_MSG_DIST_LEN, line);
    if (x->len == 0) dan_error(DAN_MSG_STAT_EMPTY, line);
    double s = 0, ps = 0;
    for (int64_t i = 0; i < x->len; i++) {
        if (p->items[i] < 0) dan_error(DAN_MSG_P_RANGE, line);
        s += x->items[i] * p->items[i]; ps += p->items[i];
    }
    if (fabs(ps - 1) > 1e-9) dan_error(DAN_MSG_DIST_SUM, line);
    return s;
}
static dan_rat dan_var_dist_rat(const dl_Lr *x, const dl_Lr *p, int line) {
    dan_rat m = dan_expect_rat(x, p, line), s = { 0, 1 };
    for (int64_t i = 0; i < x->len; i++) {
        dan_rat dlt = dan_rat_sub(x->items[i], m, line);
        s = dan_rat_add(s, dan_rat_mul(p->items[i], dan_rat_mul(dlt, dlt, line), line), line);
    }
    return s;
}
static dl_Lr *dan_ints_to_rats(const dl_Li *l, int line) {
    dl_Lr *r = dl_Lr_from(0, NULL);
    for (int64_t i = 0; i < l->len; i++) dl_Lr_push(r, dan_rat_from_int(l->items[i], line));
    return r;
}
static dl_Lf *dan_ints_to_floats(const dl_Li *l) {
    dl_Lf *r = dl_Lf_from(0, NULL);
    for (int64_t i = 0; i < l->len; i++) dl_Lf_push(r, (double)l->items[i]);
    return r;
}
static dan_rat dan_var_core_rat(const dl_Lr *l, int64_t minus, int line) {
    if (l->len < 1 + minus) dan_error(minus ? DAN_MSG_STAT_TWO : DAN_MSG_STAT_EMPTY, line);
    dan_rat s = { 0, 1 };
    for (int64_t i = 0; i < l->len; i++) s = dan_rat_add(s, l->items[i], line);
    dan_rat m = dan_rat_div(s, dan_rat_from_int(l->len, line), line), q = { 0, 1 };
    for (int64_t i = 0; i < l->len; i++) {
        dan_rat dlt = dan_rat_sub(l->items[i], m, line);
        q = dan_rat_add(q, dan_rat_mul(dlt, dlt, line), line);
    }
    return dan_rat_div(q, dan_rat_from_int(l->len - minus, line), line);
}
static double dan_var_core_float(const dl_Lf *l, int64_t minus, int line) {
    if (l->len < 1 + minus) dan_error(minus ? DAN_MSG_STAT_TWO : DAN_MSG_STAT_EMPTY, line);
    double m = 0;
    for (int64_t i = 0; i < l->len; i++) m += l->items[i];
    m /= (double)l->len;
    double q = 0;
    for (int64_t i = 0; i < l->len; i++) q += (l->items[i] - m) * (l->items[i] - m);
    return q / (double)(l->len - minus);
}
static dan_rat dan_var_rat(const dl_Lr *l, int line) { return dan_var_core_rat(l, 0, line); }
static dan_rat dan_svar_rat(const dl_Lr *l, int line) { return dan_var_core_rat(l, 1, line); }
static dan_rat dan_var_rat_i(const dl_Li *l, int line) { return dan_var_core_rat(dan_ints_to_rats(l, line), 0, line); }
static dan_rat dan_svar_rat_i(const dl_Li *l, int line) { return dan_var_core_rat(dan_ints_to_rats(l, line), 1, line); }
static double dan_var_float(const dl_Lf *l, int line) { return dan_var_core_float(l, 0, line); }
static double dan_svar_float(const dl_Lf *l, int line) { return dan_var_core_float(l, 1, line); }
static double dan_sd_float(const dl_Lf *l, int line) { return sqrt(dan_var_core_float(l, 0, line)); }
static double dan_sd_rat(const dl_Lr *l, int line) { return sqrt(dan_rat_to_float(dan_var_core_rat(l, 0, line))); }
static double dan_sd_i(const dl_Li *l, int line) { return dan_sd_float(dan_ints_to_floats(l), line); }
static void dan_series_check(const dl_Lf *x, const dl_Lf *y, int line) {
    if (x->len != y->len) dan_error(DAN_MSG_SERIES_LEN, line);
    if (x->len < 2) dan_error(DAN_MSG_STAT_TWO, line);
}
static void dan_moments(const dl_Lf *x, const dl_Lf *y, double *sxx, double *syy, double *sxy, double *mx, double *my) {
    double n = (double)x->len, ax = 0, ay = 0;
    for (int64_t i = 0; i < x->len; i++) { ax += x->items[i]; ay += y->items[i]; }
    ax /= n; ay /= n;
    double a = 0, b = 0, c = 0;
    for (int64_t i = 0; i < x->len; i++) {
        double dx = x->items[i] - ax, dy = y->items[i] - ay;
        a += dx * dx; b += dy * dy; c += dx * dy;
    }
    *sxx = a; *syy = b; *sxy = c; *mx = ax; *my = ay;
}
static double dan_corr_float(const dl_Lf *x, const dl_Lf *y, int line) {
    dan_series_check(x, y, line);
    double a, b, c, mx, my;
    dan_moments(x, y, &a, &b, &c, &mx, &my);
    if (a == 0 || b == 0) dan_error(DAN_MSG_CONST_SERIES, line);
    return c / sqrt(a * b);
}
static double dan_corr_i(const dl_Li *x, const dl_Li *y, int line) { return dan_corr_float(dan_ints_to_floats(x), dan_ints_to_floats(y), line); }
static dl_Lf *dan_linreg_float(const dl_Lf *x, const dl_Lf *y, int line) {
    dan_series_check(x, y, line);
    double a, b, c, mx, my;
    dan_moments(x, y, &a, &b, &c, &mx, &my);
    if (a == 0) dan_error(DAN_MSG_CONST_SERIES, line);
    double k = c / a;
    double kb[2] = { k, my - k * mx };
    return dl_Lf_from(2, kb);
}
static dl_Lf *dan_linreg_i(const dl_Li *x, const dl_Li *y, int line) { return dan_linreg_float(dan_ints_to_floats(x), dan_ints_to_floats(y), line); }
static double dan_entropy(const dl_Lf *p, int line) {
    if (p->len == 0) dan_error(DAN_MSG_STAT_EMPTY, line);
    double s = 0, h = 0;
    for (int64_t i = 0; i < p->len; i++) {
        if (p->items[i] < 0) dan_error(DAN_MSG_P_RANGE, line);
        s += p->items[i];
        if (p->items[i] > 0) h -= p->items[i] * log2(p->items[i]);
    }
    if (fabs(s - 1) > 1e-9) dan_error(DAN_MSG_DIST_SUM, line);
    return h + 0.0;
}

/* random numbers: xoshiro256** seeded from the clock once per run */
static DAN_TLS uint64_t dan_rng_s[4];
static DAN_TLS bool dan_rng_ready = false;
static uint64_t dan_rng_mix(uint64_t *x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static uint64_t dan_rng_next(void) {
    if (!dan_rng_ready) {
        uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)clock() << 32) ^ (uint64_t)(uintptr_t)&seed;
        for (int i = 0; i < 4; i++) dan_rng_s[i] = dan_rng_mix(&seed);
        dan_rng_ready = true;
    }
    uint64_t *s = dan_rng_s;
    uint64_t r = ((s[1] * 5) << 7 | (s[1] * 5) >> 57) * 9, t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
    s[3] = (s[3] << 45) | (s[3] >> 19);
    return r;
}
static uint64_t dan_rng_below(uint64_t n) {   /* равномерно от 0 до n - 1, n > 0 */
    uint64_t lim = UINT64_MAX - UINT64_MAX % n, v;
    do v = dan_rng_next(); while (v >= lim);
    return v % n;
}
static int64_t dan_random_int(int64_t a, int64_t b, int line) {
    if (a > b) dan_error(DAN_MSG_RANDOM_RANGE, line);
    uint64_t span = (uint64_t)b - (uint64_t)a;
    if (span == UINT64_MAX) return (int64_t)dan_rng_next();
    return (int64_t)((uint64_t)a + dan_rng_below(span + 1));
}
static double dan_random_float(void) { return (double)(dan_rng_next() >> 11) * 0x1.0p-53; }
#define DAN_SHUFFLE(S, s) \
static S *dan_shuffle_##s(const S *l) { \
    S *r = S##_from(l->len, l->items); \
    for (int64_t i = r->len - 1; i > 0; i--) { \
        int64_t j = (int64_t)dan_rng_below((uint64_t)i + 1); \
        __auto_type t = r->items[i]; r->items[i] = r->items[j]; r->items[j] = t; \
    } \
    return r; \
}
DAN_SHUFFLE(dl_Li, i)
DAN_SHUFFLE(dl_Ls, s)
DAN_SHUFFLE(dl_Lr, r)
DAN_SHUFFLE(dl_Lf, f)

/* -- linear algebra -- */
static int64_t dan_mat_cols_r(const dl_LLr *m, int line) {
    if (m->len == 0 || m->items[0]->len == 0) dan_error(DAN_MSG_MATRIX_EMPTY, line);
    for (int64_t i = 1; i < m->len; i++) if (m->items[i]->len != m->items[0]->len) dan_error(DAN_MSG_MATRIX_RECT, line);
    return m->items[0]->len;
}
static int64_t dan_mat_cols_f(const dl_LLf *m, int line) {
    if (m->len == 0 || m->items[0]->len == 0) dan_error(DAN_MSG_MATRIX_EMPTY, line);
    for (int64_t i = 1; i < m->len; i++) if (m->items[i]->len != m->items[0]->len) dan_error(DAN_MSG_MATRIX_RECT, line);
    return m->items[0]->len;
}
/* рабочая копия матрицы в одном массиве */
static dan_rat *dan_mat_flat_r(const dl_LLr *m, int64_t c) {
    dan_rat *a = dan_alloc(sizeof(dan_rat) * (size_t)(m->len * c));
    for (int64_t i = 0; i < m->len; i++) for (int64_t j = 0; j < c; j++) a[i * c + j] = m->items[i]->items[j];
    return a;
}
static double *dan_mat_flat_f(const dl_LLf *m, int64_t c) {
    double *a = dan_alloc(sizeof(double) * (size_t)(m->len * c));
    for (int64_t i = 0; i < m->len; i++) for (int64_t j = 0; j < c; j++) a[i * c + j] = m->items[i]->items[j];
    return a;
}
static dl_LLr *dan_mat_from_r(const dan_rat *a, int64_t r, int64_t c) {
    dl_LLr *m = dl_LLr_from(0, NULL);
    for (int64_t i = 0; i < r; i++) dl_LLr_push(m, dl_Lr_from(c, a + i * c));
    return m;
}
static dl_LLf *dan_mat_from_f(const double *a, int64_t r, int64_t c) {
    dl_LLf *m = dl_LLf_from(0, NULL);
    for (int64_t i = 0; i < r; i++) dl_LLf_push(m, dl_Lf_from(c, a + i * c));
    return m;
}
/* Приведение к ступенчатому виду; возвращает ранг, в *det - определитель (для квадратной). */
static int64_t dan_gauss_r(dan_rat *a, int64_t r, int64_t c, dan_rat *det, dan_rat *aug, int64_t ac, int line) {
    int64_t rank = 0;
    dan_rat d = { 1, 1 };
    for (int64_t col = 0; col < c && rank < r; col++) {
        int64_t p = -1;
        for (int64_t i = rank; i < r; i++) if (a[i * c + col].n != 0) { p = i; break; }
        if (p < 0) { d.n = 0; d.d = 1; continue; }
        if (p != rank) {
            for (int64_t j = 0; j < c; j++) { dan_rat t = a[p * c + j]; a[p * c + j] = a[rank * c + j]; a[rank * c + j] = t; }
            for (int64_t j = 0; j < ac; j++) { dan_rat t = aug[p * ac + j]; aug[p * ac + j] = aug[rank * ac + j]; aug[rank * ac + j] = t; }
            d = dan_rat_neg(d, line);
        }
        dan_rat pv = a[rank * c + col];
        d = dan_rat_mul(d, pv, line);
        for (int64_t j = 0; j < c; j++) a[rank * c + j] = dan_rat_div(a[rank * c + j], pv, line);
        for (int64_t j = 0; j < ac; j++) aug[rank * ac + j] = dan_rat_div(aug[rank * ac + j], pv, line);
        for (int64_t i = 0; i < r; i++) {
            if (i == rank || a[i * c + col].n == 0) continue;
            dan_rat f = a[i * c + col];
            for (int64_t j = 0; j < c; j++) a[i * c + j] = dan_rat_sub(a[i * c + j], dan_rat_mul(f, a[rank * c + j], line), line);
            for (int64_t j = 0; j < ac; j++) aug[i * ac + j] = dan_rat_sub(aug[i * ac + j], dan_rat_mul(f, aug[rank * ac + j], line), line);
        }
        rank++;
    }
    if (rank < r || rank < c) { d.n = 0; d.d = 1; }
    if (det) *det = d;
    return rank;
}
static dan_rat dan_det_rat(const dl_LLr *m, int line) {
    int64_t c = dan_mat_cols_r(m, line);
    if (c != m->len) dan_error(DAN_MSG_MATRIX_SQUARE, line);
    dan_rat d;
    dan_gauss_r(dan_mat_flat_r(m, c), c, c, &d, NULL, 0, line);
    return d;
}
static int64_t dan_rank_rat(const dl_LLr *m, int line) {
    int64_t c = dan_mat_cols_r(m, line);
    return dan_gauss_r(dan_mat_flat_r(m, c), m->len, c, NULL, NULL, 0, line);
}
static dl_LLr *dan_identity(int64_t n, int line) {
    if (n < 1) dan_error(DAN_MSG_MATRIX_EMPTY, line);
    dan_rat *a = dan_alloc(sizeof(dan_rat) * (size_t)(n * n));
    for (int64_t i = 0; i < n * n; i++) { a[i].n = (i / n == i % n); a[i].d = 1; }
    return dan_mat_from_r(a, n, n);
}
static dl_LLr *dan_inverse_rat(const dl_LLr *m, int line) {
    int64_t c = dan_mat_cols_r(m, line);
    if (c != m->len) dan_error(DAN_MSG_MATRIX_SQUARE, line);
    dan_rat *a = dan_mat_flat_r(m, c), *e = dan_alloc(sizeof(dan_rat) * (size_t)(c * c));
    for (int64_t i = 0; i < c * c; i++) { e[i].n = (i / c == i % c); e[i].d = 1; }
    if (dan_gauss_r(a, c, c, NULL, e, c, line) < c) dan_error(DAN_MSG_MATRIX_SINGULAR, line);
    return dan_mat_from_r(e, c, c);
}
static double dan_gauss_f(double *a, int64_t n, double *aug, int line) {   /* квадратная, частичный выбор главного */
    (void)line;
    double d = 1;
    for (int64_t col = 0; col < n; col++) {
        int64_t p = col;
        for (int64_t i = col + 1; i < n; i++) if (fabs(a[i * n + col]) > fabs(a[p * n + col])) p = i;
        if (a[p * n + col] == 0) return 0;
        if (p != col) {
            for (int64_t j = 0; j < n; j++) { double t = a[p * n + j]; a[p * n + j] = a[col * n + j]; a[col * n + j] = t; }
            if (aug) for (int64_t j = 0; j < n; j++) { double t = aug[p * n + j]; aug[p * n + j] = aug[col * n + j]; aug[col * n + j] = t; }
            d = -d;
        }
        double pv = a[col * n + col];
        d *= pv;
        for (int64_t j = 0; j < n; j++) a[col * n + j] /= pv;
        if (aug) for (int64_t j = 0; j < n; j++) aug[col * n + j] /= pv;
        for (int64_t i = 0; i < n; i++) {
            if (i == col || a[i * n + col] == 0) continue;
            double f = a[i * n + col];
            for (int64_t j = 0; j < n; j++) a[i * n + j] -= f * a[col * n + j];
            if (aug) for (int64_t j = 0; j < n; j++) aug[i * n + j] -= f * aug[col * n + j];
        }
    }
    return d;
}
static double dan_det_float(const dl_LLf *m, int line) {
    int64_t c = dan_mat_cols_f(m, line);
    if (c != m->len) dan_error(DAN_MSG_MATRIX_SQUARE, line);
    return dan_gauss_f(dan_mat_flat_f(m, c), c, NULL, line);
}
static dl_LLf *dan_inverse_float(const dl_LLf *m, int line) {
    int64_t c = dan_mat_cols_f(m, line);
    if (c != m->len) dan_error(DAN_MSG_MATRIX_SQUARE, line);
    double *a = dan_mat_flat_f(m, c), *e = dan_alloc(sizeof(double) * (size_t)(c * c));
    for (int64_t i = 0; i < c * c; i++) e[i] = (i / c == i % c);
    if (dan_gauss_f(a, c, e, line) == 0) dan_error(DAN_MSG_MATRIX_SINGULAR, line);
    return dan_mat_from_f(e, c, c);
}
static dl_LLr *dan_transpose_rat(const dl_LLr *m, int line) {
    int64_t c = dan_mat_cols_r(m, line), r = m->len;
    dan_rat *a = dan_alloc(sizeof(dan_rat) * (size_t)(r * c));
    for (int64_t i = 0; i < r; i++) for (int64_t j = 0; j < c; j++) a[j * r + i] = m->items[i]->items[j];
    return dan_mat_from_r(a, c, r);
}
static dl_LLf *dan_transpose_float(const dl_LLf *m, int line) {
    int64_t c = dan_mat_cols_f(m, line), r = m->len;
    double *a = dan_alloc(sizeof(double) * (size_t)(r * c));
    for (int64_t i = 0; i < r; i++) for (int64_t j = 0; j < c; j++) a[j * r + i] = m->items[i]->items[j];
    return dan_mat_from_f(a, c, r);
}
static dl_LLr *dan_matmul_rat(const dl_LLr *x, const dl_LLr *y, int line) {
    int64_t n = dan_mat_cols_r(x, line), k = dan_mat_cols_r(y, line);
    if (n != y->len) dan_error(DAN_MSG_MATRIX_DIMS, line);
    dan_rat *a = dan_alloc(sizeof(dan_rat) * (size_t)(x->len * k));
    for (int64_t i = 0; i < x->len; i++)
        for (int64_t j = 0; j < k; j++) {
            dan_rat s = { 0, 1 };
            for (int64_t t = 0; t < n; t++) s = dan_rat_add(s, dan_rat_mul(x->items[i]->items[t], y->items[t]->items[j], line), line);
            a[i * k + j] = s;
        }
    return dan_mat_from_r(a, x->len, k);
}
static dl_LLf *dan_matmul_float(const dl_LLf *x, const dl_LLf *y, int line) {
    int64_t n = dan_mat_cols_f(x, line), k = dan_mat_cols_f(y, line);
    if (n != y->len) dan_error(DAN_MSG_MATRIX_DIMS, line);
    double *a = dan_alloc(sizeof(double) * (size_t)(x->len * k));
    for (int64_t i = 0; i < x->len; i++)
        for (int64_t j = 0; j < k; j++) {
            double s = 0;
            for (int64_t t = 0; t < n; t++) s += x->items[i]->items[t] * y->items[t]->items[j];
            a[i * k + j] = s;
        }
    return dan_mat_from_f(a, x->len, k);
}
static dl_LLr *dan_matadd_rat(const dl_LLr *x, const dl_LLr *y, int line) {
    int64_t c = dan_mat_cols_r(x, line);
    if (c != dan_mat_cols_r(y, line) || x->len != y->len) dan_error(DAN_MSG_MATRIX_DIMS, line);
    dan_rat *a = dan_alloc(sizeof(dan_rat) * (size_t)(x->len * c));
    for (int64_t i = 0; i < x->len; i++) for (int64_t j = 0; j < c; j++)
        a[i * c + j] = dan_rat_add(x->items[i]->items[j], y->items[i]->items[j], line);
    return dan_mat_from_r(a, x->len, c);
}
static dl_LLf *dan_matadd_float(const dl_LLf *x, const dl_LLf *y, int line) {
    int64_t c = dan_mat_cols_f(x, line);
    if (c != dan_mat_cols_f(y, line) || x->len != y->len) dan_error(DAN_MSG_MATRIX_DIMS, line);
    double *a = dan_alloc(sizeof(double) * (size_t)(x->len * c));
    for (int64_t i = 0; i < x->len; i++) for (int64_t j = 0; j < c; j++) a[i * c + j] = x->items[i]->items[j] + y->items[i]->items[j];
    return dan_mat_from_f(a, x->len, c);
}
static dan_rat dan_dot_rat(const dl_Lr *a, const dl_Lr *b, int line) {
    if (a->len != b->len) dan_error(DAN_MSG_VECTOR_LEN, line);
    dan_rat s = { 0, 1 };
    for (int64_t i = 0; i < a->len; i++) s = dan_rat_add(s, dan_rat_mul(a->items[i], b->items[i], line), line);
    return s;
}
static double dan_dot_float(const dl_Lf *a, const dl_Lf *b, int line) {
    if (a->len != b->len) dan_error(DAN_MSG_VECTOR_LEN, line);
    double s = 0;
    for (int64_t i = 0; i < a->len; i++) s += a->items[i] * b->items[i];
    return s;
}
static dl_Lr *dan_cross_rat(const dl_Lr *a, const dl_Lr *b, int line) {
    if (a->len != 3 || b->len != 3) dan_error(DAN_MSG_VECTOR_3, line);
    const dan_rat *x = a->items, *y = b->items;
    dan_rat r[3] = {
        dan_rat_sub(dan_rat_mul(x[1], y[2], line), dan_rat_mul(x[2], y[1], line), line),
        dan_rat_sub(dan_rat_mul(x[2], y[0], line), dan_rat_mul(x[0], y[2], line), line),
        dan_rat_sub(dan_rat_mul(x[0], y[1], line), dan_rat_mul(x[1], y[0], line), line) };
    return dl_Lr_from(3, r);
}
static dl_Lf *dan_cross_float(const dl_Lf *a, const dl_Lf *b, int line) {
    if (a->len != 3 || b->len != 3) dan_error(DAN_MSG_VECTOR_3, line);
    const double *x = a->items, *y = b->items;
    double r[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
    return dl_Lf_from(3, r);
}
static double dan_norm_float(const dl_Lf *a) { double s = 0; for (int64_t i = 0; i < a->len; i++) s += a->items[i] * a->items[i]; return sqrt(s); }
static double dan_norm_rat(const dl_Lr *a) {
    double s = 0;
    for (int64_t i = 0; i < a->len; i++) { double v = dan_rat_to_float(a->items[i]); s += v * v; }
    return sqrt(s);
}
static double dan_angle_float(const dl_Lf *a, const dl_Lf *b, int line) {
    double na = dan_norm_float(a), nb = dan_norm_float(b);
    if (na == 0 || nb == 0) dan_error(DAN_MSG_ZERO_VECTOR, line);
    double c = dan_dot_float(a, b, line) / (na * nb);
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    return acos(c);
}

/* -- analytic geometry -- */
static double dan_dist_float(double x1, double y1, double x2, double y2, int line) { (void)line; return hypot(x2 - x1, y2 - y1); }
static double dan_dist_rat(dan_rat x1, dan_rat y1, dan_rat x2, dan_rat y2, int line) {
    dan_rat dx = dan_rat_sub(x2, x1, line), dy = dan_rat_sub(y2, y1, line);
    return sqrt(dan_rat_to_float(dan_rat_add(dan_rat_mul(dx, dx, line), dan_rat_mul(dy, dy, line), line)));
}
static dl_Lr *dan_midpoint_rat(dan_rat x1, dan_rat y1, dan_rat x2, dan_rat y2, int line) {
    dan_rat two = dan_rat_from_int(2, line);
    dan_rat r[2] = { dan_rat_div(dan_rat_add(x1, x2, line), two, line), dan_rat_div(dan_rat_add(y1, y2, line), two, line) };
    return dl_Lr_from(2, r);
}
static dl_Lf *dan_midpoint_float(double x1, double y1, double x2, double y2, int line) {
    (void)line;
    double r[2] = { (x1 + x2) / 2, (y1 + y2) / 2 };
    return dl_Lf_from(2, r);
}
static dl_Lr *dan_line_coeffs_rat(dan_rat x1, dan_rat y1, dan_rat x2, dan_rat y2, int line) {
    if (dan_rat_cmp(x1, x2) == 0 && dan_rat_cmp(y1, y2) == 0) dan_error(DAN_MSG_SAME_POINTS, line);
    dan_rat r[3] = { dan_rat_sub(y2, y1, line), dan_rat_sub(x1, x2, line),
                     dan_rat_sub(dan_rat_mul(x2, y1, line), dan_rat_mul(x1, y2, line), line) };
    if (r[0].n < 0 || (r[0].n == 0 && r[1].n < 0)) for (int i = 0; i < 3; i++) r[i] = dan_rat_neg(r[i], line);
    return dl_Lr_from(3, r);
}
static dl_Lf *dan_line_coeffs_float(double x1, double y1, double x2, double y2, int line) {
    if (x1 == x2 && y1 == y2) dan_error(DAN_MSG_SAME_POINTS, line);
    double r[3] = { y2 - y1, x1 - x2, x2 * y1 - x1 * y2 };
    if (r[0] < 0 || (r[0] == 0 && r[1] < 0)) for (int i = 0; i < 3; i++) r[i] = -r[i];
    return dl_Lf_from(3, r);
}
static dan_str dan_line_str(dan_rat x1, dan_rat y1, dan_rat x2, dan_rat y2, int line) {
    if (dan_rat_cmp(x1, x2) == 0 && dan_rat_cmp(y1, y2) == 0) dan_error(DAN_MSG_SAME_POINTS, line);
    dan_buf b = { NULL, 0, 0 };
    if (dan_rat_cmp(x1, x2) == 0) {
        dan_buf_add(&b, "\321\205 = ");      /* «х = » */
        dan_buf_add(&b, dan_rat_to_str(x1));
        return dan_buf_str(&b);
    }
    dan_rat k = dan_rat_div(dan_rat_sub(y2, y1, line), dan_rat_sub(x2, x1, line), line);
    dan_rat c = dan_rat_sub(y1, dan_rat_mul(k, x1, line), line);
    dan_mn p = dan_poly_add(dan_poly_scale(dan_poly_x(), k, line), dan_poly_const(c), line);
    dan_buf_add(&b, "\321\203 = ");          /* «у = » */
    dan_buf_add(&b, dan_poly_to_str(p));
    return dan_buf_str(&b);
}
static do_Lr dan_line_intersection(dan_rat a1, dan_rat b1, dan_rat c1, dan_rat a2, dan_rat b2, dan_rat c2, int line) {
    dan_rat d = dan_rat_sub(dan_rat_mul(a1, b2, line), dan_rat_mul(a2, b1, line), line);
    if (d.n == 0) return do_Lr_none();
    dan_rat x = dan_rat_div(dan_rat_sub(dan_rat_mul(b1, c2, line), dan_rat_mul(b2, c1, line), line), d, line);
    dan_rat y = dan_rat_div(dan_rat_sub(dan_rat_mul(c1, a2, line), dan_rat_mul(c2, a1, line), line), d, line);
    dan_rat r[2] = { x, y };
    return do_Lr_some(dl_Lr_from(2, r));
}
static dan_rat dan_tri_area_rat(dan_rat x1, dan_rat y1, dan_rat x2, dan_rat y2, dan_rat x3, dan_rat y3, int line) {
    dan_rat s = dan_rat_sub(dan_rat_mul(dan_rat_sub(x2, x1, line), dan_rat_sub(y3, y1, line), line),
                            dan_rat_mul(dan_rat_sub(x3, x1, line), dan_rat_sub(y2, y1, line), line), line);
    if (s.n < 0) s = dan_rat_neg(s, line);
    return dan_rat_div(s, dan_rat_from_int(2, line), line);
}
static double dan_tri_area_float(double x1, double y1, double x2, double y2, double x3, double y3) {
    return fabs((x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1)) / 2;
}
static dan_rat dan_polygon_area(const dl_Lr *x, const dl_Lr *y, int line) {
    if (x->len != y->len || x->len < 3) dan_error(DAN_MSG_POLYGON, line);
    dan_rat s = { 0, 1 };
    for (int64_t i = 0; i < x->len; i++) {
        int64_t j = (i + 1) % x->len;
        s = dan_rat_add(s, dan_rat_sub(dan_rat_mul(x->items[i], y->items[j], line), dan_rat_mul(x->items[j], y->items[i], line), line), line);
    }
    if (s.n < 0) s = dan_rat_neg(s, line);
    return dan_rat_div(s, dan_rat_from_int(2, line), line);
}

/* -- planimetry and stereometry -- */
#define DAN_PI 3.141592653589793
static void dan_nonneg(double v, int line) { if (v < 0) dan_error(DAN_MSG_NEG_LENGTH, line); }
static void dan_nonneg_r(dan_rat v, int line) { if (v.n < 0) dan_error(DAN_MSG_NEG_LENGTH, line); }
static double dan_hypot(double a, double b, int line) { dan_nonneg(a, line); dan_nonneg(b, line); return hypot(a, b); }
static double dan_leg(double c, double a, int line) { if (!(a > 0 && c > a)) dan_error(DAN_MSG_LEG, line); return sqrt((c - a) * (c + a)); }
static double dan_heron(double a, double b, double c, int line) {
    if (!(a > 0 && b > 0 && c > 0 && a + b > c && a + c > b && b + c > a)) dan_error(DAN_MSG_TRIANGLE, line);
    double p = (a + b + c) / 2;
    return sqrt(p * (p - a) * (p - b) * (p - c));
}
static dan_rat dan_half_r(dan_rat v, int line) { return dan_rat_div(v, dan_rat_from_int(2, line), line); }
static dan_rat dan_trapezoid_rat(dan_rat a, dan_rat b, dan_rat h, int line) {
    dan_nonneg_r(a, line); dan_nonneg_r(b, line); dan_nonneg_r(h, line);
    return dan_half_r(dan_rat_mul(dan_rat_add(a, b, line), h, line), line);
}
static double dan_trapezoid_float(double a, double b, double h) { return (a + b) * h / 2; }
static dan_rat dan_parallelogram_rat(dan_rat a, dan_rat h, int line) { dan_nonneg_r(a, line); dan_nonneg_r(h, line); return dan_rat_mul(a, h, line); }
static double dan_parallelogram_float(double a, double h) { return a * h; }
static dan_rat dan_rhombus_rat(dan_rat d1, dan_rat d2, int line) { dan_nonneg_r(d1, line); dan_nonneg_r(d2, line); return dan_half_r(dan_rat_mul(d1, d2, line), line); }
static double dan_rhombus_float(double d1, double d2) { return d1 * d2 / 2; }
static dan_rat dan_pyramid_rat(dan_rat s, dan_rat h, int line) {
    dan_nonneg_r(s, line); dan_nonneg_r(h, line);
    return dan_rat_div(dan_rat_mul(s, h, line), dan_rat_from_int(3, line), line);
}
static double dan_pyramid_float(double s, double h) { return s * h / 3; }
static dan_rat dan_prism_rat(dan_rat s, dan_rat h, int line) { dan_nonneg_r(s, line); dan_nonneg_r(h, line); return dan_rat_mul(s, h, line); }
static double dan_prism_float(double s, double h) { return s * h; }
static dan_rat dan_cube_surface_rat(dan_rat a, int line) { dan_nonneg_r(a, line); return dan_rat_mul(dan_rat_from_int(6, line), dan_rat_mul(a, a, line), line); }
static double dan_cube_surface_float(double a) { return 6 * a * a; }
static double dan_ball_volume(double r) { return 4.0 / 3.0 * DAN_PI * r * r * r; }
static double dan_sphere_area(double r) { return 4 * DAN_PI * r * r; }
static double dan_cyl_volume(double r, double h) { return DAN_PI * r * r * h; }
static double dan_cyl_area(double r, double h) { return 2 * DAN_PI * r * (r + h); }
static double dan_cone_volume(double r, double h) { return DAN_PI * r * r * h / 3; }
static double dan_arc_length(double r, double deg) { return DAN_PI * r * deg / 180; }
static double dan_sector_area(double r, double deg) { return DAN_PI * r * r * deg / 360; }
static int64_t dan_polygon_angles(int64_t n, int line) {
    if (n < 3) dan_error(DAN_MSG_POLYGON_N, line);
    return dan_mul_chk(180, n - 2, line);
}

/* -- number systems, words -- */
static dan_str dan_to_base(int64_t v, int64_t base, int line) {
    if (base < 2 || base > 36) dan_error(DAN_MSG_BASE, line);
    char buf[70]; int k = 69; buf[k] = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do { buf[--k] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[u % (uint64_t)base]; u /= (uint64_t)base; } while (u);
    if (v < 0) buf[--k] = '-';
    return dan_strdup(buf + k);
}
static int64_t dan_from_base(dan_str s, int64_t base, int line) {
    if (base < 2 || base > 36) dan_error(DAN_MSG_BASE, line);
    const char *p = s;
    while (*p == ' ') p++;
    bool neg = false;
    if (*p == '-' || *p == '+') { neg = *p == '-'; p++; }
    if (!*p) dan_error(DAN_MSG_BAD_DIGIT, line);
    dan_u128 r = 0;
    for (; *p && *p != ' '; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else if (*p == '_') continue;
        else d = 99;
        if (d >= base) dan_error(DAN_MSG_BAD_DIGIT, line);
        r = r * (dan_u128)base + (dan_u128)d;
        if (r > (dan_u128)INT64_MAX + 1) dan_error(DAN_MSG_TOO_BIG, line);
    }
    while (*p == ' ') p++;
    if (*p) dan_error(DAN_MSG_BAD_DIGIT, line);
    if (!neg && r > (dan_u128)INT64_MAX) dan_error(DAN_MSG_TOO_BIG, line);
    return neg ? (int64_t)(0 - (uint64_t)r) : (int64_t)r;
}
static const int dan_roman_v[13] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
static const char *const dan_roman_s[13] = { "M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I" };
static dan_str dan_to_roman(int64_t n, int line) {
    if (n < 1 || n > 3999) dan_error(DAN_MSG_ROMAN_RANGE, line);
    dan_buf b = { NULL, 0, 0 };
    for (int i = 0; i < 13; i++) while (n >= dan_roman_v[i]) { dan_buf_add(&b, dan_roman_s[i]); n -= dan_roman_v[i]; }
    return dan_buf_str(&b);
}
static int64_t dan_from_roman(dan_str s, int line) {
    const char *p = s;
    int64_t r = 0;
    for (int i = 0; i < 13; i++) {
        size_t k = strlen(dan_roman_s[i]);
        while (strncmp(p, dan_roman_s[i], k) == 0) { p += k; r += dan_roman_v[i]; }
    }
    /* принимается только каноническая запись: IIII, VV, IC и т.п. отвергаются */
    if (*p || r < 1 || r > 3999 || strcmp(dan_to_roman(r, line), s) != 0) dan_error(DAN_MSG_BAD_ROMAN, line);
    return r;
}
static int64_t dan_powmod(int64_t a, int64_t e, int64_t m, int line) {
    if (m <= 0 || e < 0) dan_error(DAN_MSG_MOD, line);
    dan_u128 r = 1 % (dan_u128)m, b = (dan_u128)(((a % m) + m) % m);
    while (e) { if (e & 1) r = r * b % (dan_u128)m; b = b * b % (dan_u128)m; e >>= 1; }
    return (int64_t)r;
}
static do_i dan_invmod(int64_t a, int64_t m, int line) {
    if (m <= 0) dan_error(DAN_MSG_MOD, line);
    dan_i128 t = 0, nt = 1, r = m, nr = ((a % m) + m) % m;
    while (nr != 0) {
        dan_i128 q = r / nr, tmp;
        tmp = t - q * nt; t = nt; nt = tmp;
        tmp = r - q * nr; r = nr; nr = tmp;
    }
    if (r != 1) return m == 1 ? do_i_some(0) : do_i_none();
    if (t < 0) t += m;
    return do_i_some((int64_t)t);
}
static int64_t dan_fibonacci(int64_t n, int line) {
    if (n < 0 || n > 92) dan_error(DAN_MSG_FIB, line);
    int64_t a = 0, b = 1;
    for (int64_t i = 0; i < n; i++) { int64_t t = a + b; a = b; b = t; }
    return a;
}
"""

# Числа прописью: слова вставляются как строки C в восьмеричной записи.
_ЕДИНИЦЫ_М = ["", "один", "два", "три", "четыре", "пять", "шесть", "семь", "восемь", "девять"]
_ЕДИНИЦЫ_Ж = ["", "одна", "две", "три", "четыре", "пять", "шесть", "семь", "восемь", "девять"]
_ДЕСЯТЬ_19 = ["десять", "одиннадцать", "двенадцать", "тринадцать", "четырнадцать", "пятнадцать",
              "шестнадцать", "семнадцать", "восемнадцать", "девятнадцать"]
_ДЕСЯТКИ = ["", "", "двадцать", "тридцать", "сорок", "пятьдесят", "шестьдесят", "семьдесят",
            "восемьдесят", "девяносто"]
_СОТНИ = ["", "сто", "двести", "триста", "четыреста", "пятьсот", "шестьсот", "семьсот",
          "восемьсот", "девятьсот"]
# (род: 1 - женский, 0 - мужской; формы: 1, 2-4, 5-20)
_РАЗРЯДЫ = [(0, "", "", ""), (1, "тысяча", "тысячи", "тысяч"), (0, "миллион", "миллиона", "миллионов"),
            (0, "миллиард", "миллиарда", "миллиардов"), (0, "триллион", "триллиона", "триллионов"),
            (0, "квадриллион", "квадриллиона", "квадриллионов"),
            (0, "квинтиллион", "квинтиллиона", "квинтиллионов")]


def _массив_c(имя, слова):
    return f"static const char *const {имя}[] = {{ {', '.join(м._строка_c(с) for с in слова)} }};"


КОД_C += "\n".join([
    _массив_c("dan_w_ones_m", _ЕДИНИЦЫ_М), _массив_c("dan_w_ones_f", _ЕДИНИЦЫ_Ж),
    _массив_c("dan_w_teens", _ДЕСЯТЬ_19), _массив_c("dan_w_tens", _ДЕСЯТКИ), _массив_c("dan_w_hundreds", _СОТНИ),
    "static const int dan_w_gender[] = { " + ", ".join(str(р[0]) for р in _РАЗРЯДЫ) + " };",
    _массив_c("dan_w_scale1", [р[1] for р in _РАЗРЯДЫ]), _массив_c("dan_w_scale2", [р[2] for р in _РАЗРЯДЫ]),
    _массив_c("dan_w_scale5", [р[3] for р in _РАЗРЯДЫ]),
    "#define DAN_W_ZERO " + м._строка_c("ноль"),
    "#define DAN_W_MINUS " + м._строка_c("минус "),
    "#define DAN_W_RUB1 " + м._строка_c("рубль"), "#define DAN_W_RUB2 " + м._строка_c("рубля"),
    "#define DAN_W_RUB5 " + м._строка_c("рублей"), "#define DAN_W_KOP1 " + м._строка_c("копейка"),
    "#define DAN_W_KOP2 " + м._строка_c("копейки"), "#define DAN_W_KOP5 " + м._строка_c("копеек"),
]) + r"""
static int dan_plural_form(uint64_t n) {   /* 0: 1, 21...; 1: 2-4, 22-24...; 2: 0, 5-20... */
    uint64_t t = n % 100, u = n % 10;
    if (t >= 11 && t <= 14) return 2;
    if (u == 1) return 0;
    if (u >= 2 && u <= 4) return 1;
    return 2;
}
static dan_str dan_plural(int64_t n, dan_str f1, dan_str f2, dan_str f5) {
    uint64_t u = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
    int f = dan_plural_form(u);
    return f == 0 ? f1 : f == 1 ? f2 : f5;
}
static void dan_words_add(dan_buf *b, const char *w) {
    if (!*w) return;
    if (b->len) dan_buf_add(b, " ");
    dan_buf_add(b, w);
}
static void dan_words_triad(dan_buf *b, unsigned t, int female) {
    dan_words_add(b, dan_w_hundreds[t / 100]);
    unsigned r = t % 100;
    if (r >= 10 && r < 20) { dan_words_add(b, dan_w_teens[r - 10]); return; }
    dan_words_add(b, dan_w_tens[r / 10]);
    dan_words_add(b, female ? dan_w_ones_f[r % 10] : dan_w_ones_m[r % 10]);
}
static dan_str dan_in_words(int64_t v) {
    if (v == 0) return dan_strdup(DAN_W_ZERO);
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    unsigned tri[7]; int k = 0;
    while (u) { tri[k++] = (unsigned)(u % 1000); u /= 1000; }
    dan_buf b = { NULL, 0, 0 };
    for (int i = k - 1; i >= 0; i--) {
        if (tri[i] == 0) continue;
        dan_words_triad(&b, tri[i], dan_w_gender[i]);
        if (i > 0) {
            int f = dan_plural_form(tri[i]);
            dan_words_add(&b, f == 0 ? dan_w_scale1[i] : f == 1 ? dan_w_scale2[i] : dan_w_scale5[i]);
        }
    }
    if (v < 0) { dan_buf m = { NULL, 0, 0 }; dan_buf_add(&m, DAN_W_MINUS); dan_buf_add(&m, b.s); dan_buf_free(&b); return dan_buf_str(&m); }
    return dan_buf_str(&b);
}
static dan_str dan_money_words_kop(int64_t kop, int line) {
    if (kop < 0) dan_error(DAN_MSG_NEG_MONEY, line);
    int64_t rub = kop / 100, k = kop % 100;
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, dan_in_words(rub));
    dan_buf_add(&b, " ");
    int f = dan_plural_form((uint64_t)rub);
    dan_buf_add(&b, f == 0 ? DAN_W_RUB1 : f == 1 ? DAN_W_RUB2 : DAN_W_RUB5);
    char num[8];
    snprintf(num, sizeof num, " %02d ", (int)k);
    dan_buf_add(&b, num);
    f = dan_plural_form((uint64_t)k);
    dan_buf_add(&b, f == 0 ? DAN_W_KOP1 : f == 1 ? DAN_W_KOP2 : DAN_W_KOP5);
    return dan_buf_str(&b);
}
static int64_t dan_to_kop_rat(dan_rat v, int line) {
    if (v.n < 0) dan_error(DAN_MSG_NEG_MONEY, line);
    dan_i128 k = ((dan_i128)v.n * 200 + v.d) / (2 * (dan_i128)v.d);   /* до копеек с округлением половины вверх */
    if (k > INT64_MAX) dan_error(DAN_MSG_TOO_BIG, line);
    return (int64_t)k;
}
static dan_str dan_money_digits_kop(int64_t kop) {
    char digits[24], out[48];
    int n = snprintf(digits, sizeof digits, "%lld", (long long)(kop / 100)), k = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0) out[k++] = ' ';
        out[k++] = digits[i];
    }
    snprintf(out + k, sizeof out - (size_t)k, ",%02d", (int)(kop % 100));
    return dan_strdup(out);
}
static dan_str dan_money_digits_rat(dan_rat v, int line) { return dan_money_digits_kop(dan_to_kop_rat(v, line)); }
static dan_str dan_money_digits_float(double v, int line) {
    if (v < 0) dan_error(DAN_MSG_NEG_MONEY, line);
    return dan_money_digits_kop(dan_float_to_i64(floor(v * 100 + 0.5), line));
}
static dan_str dan_money_words_rat(dan_rat v, int line) {
    if (v.n < 0) dan_error(DAN_MSG_NEG_MONEY, line);
    dan_i128 k = ((dan_i128)v.n * 200 + v.d) / (2 * (dan_i128)v.d);   /* до копеек с округлением половины вверх */
    if (k > INT64_MAX) dan_error(DAN_MSG_TOO_BIG, line);
    return dan_money_words_kop((int64_t)k, line);
}
static dan_str dan_money_words_float(double v, int line) {
    if (v < 0) dan_error(DAN_MSG_NEG_MONEY, line);
    return dan_money_words_kop(dan_float_to_i64(floor(v * 100 + 0.5), line), line);
}

/* -- progressions -- */
static void dan_term_check(int64_t n, int line) { if (n < 1) dan_error(DAN_MSG_TERM_N, line); }
static dan_rat dan_arith_term_rat(dan_rat a, dan_rat d, int64_t n, int line) {
    dan_term_check(n, line);
    return dan_rat_add(a, dan_rat_mul(d, dan_rat_from_int(n - 1, line), line), line);
}
static double dan_arith_term_float(double a, double d, int64_t n, int line) { dan_term_check(n, line); return a + d * (double)(n - 1); }
static dan_rat dan_arith_sum_rat(dan_rat a, dan_rat d, int64_t n, int line) {
    dan_rat last = dan_arith_term_rat(a, d, n, line);
    return dan_rat_div(dan_rat_mul(dan_rat_add(a, last, line), dan_rat_from_int(n, line), line), dan_rat_from_int(2, line), line);
}
static double dan_arith_sum_float(double a, double d, int64_t n, int line) { return (a + dan_arith_term_float(a, d, n, line)) * (double)n / 2; }
static dan_rat dan_geom_term_rat(dan_rat b, dan_rat q, int64_t n, int line) { dan_term_check(n, line); return dan_rat_mul(b, dan_rat_pow(q, n - 1, line), line); }
static double dan_geom_term_float(double b, double q, int64_t n, int line) { dan_term_check(n, line); return b * pow(q, (double)(n - 1)); }
static dan_rat dan_geom_sum_rat(dan_rat b, dan_rat q, int64_t n, int line) {
    dan_term_check(n, line);
    if (q.n == q.d) return dan_rat_mul(b, dan_rat_from_int(n, line), line);
    dan_rat one = dan_rat_from_int(1, line);
    return dan_rat_div(dan_rat_mul(b, dan_rat_sub(dan_rat_pow(q, n, line), one, line), line), dan_rat_sub(q, one, line), line);
}
static double dan_geom_sum_float(double b, double q, int64_t n, int line) {
    dan_term_check(n, line);
    if (q == 1) return b * (double)n;
    return b * (pow(q, (double)n) - 1) / (q - 1);
}

/* -- financial mathematics (ставки в процентах) -- */
static dan_rat dan_pct_r(dan_rat r, int line) { return dan_rat_div(r, dan_rat_from_int(100, line), line); }
static dan_rat dan_simple_interest_rat(dan_rat s, dan_rat r, dan_rat t, int line) {
    return dan_rat_mul(s, dan_rat_add(dan_rat_from_int(1, line), dan_rat_mul(dan_pct_r(r, line), t, line), line), line);
}
static double dan_simple_interest_float(double s, double r, double t) { return s * (1 + r / 100 * t); }
static dan_rat dan_compound_rat(dan_rat s, dan_rat r, int64_t t, int line) {
    if (t < 0) dan_error(DAN_MSG_NEG_LENGTH, line);
    return dan_rat_mul(s, dan_rat_pow(dan_rat_add(dan_rat_from_int(1, line), dan_pct_r(r, line), line), t, line), line);
}
static double dan_compound_float(double s, double r, double t) { return s * pow(1 + r / 100, t); }
static double dan_compound_n(double s, double r, double t, int64_t m, int line) {
    if (m < 1) dan_error(DAN_MSG_TERM_N, line);
    return s * pow(1 + r / 100 / (double)m, t * (double)m);
}
static double dan_annuity(double s, double r, int64_t n, int line) {
    if (n < 1) dan_error(DAN_MSG_MONTHS, line);
    double i = r / 1200;
    if (i == 0) return s / (double)n;
    return s * i / (1 - pow(1 + i, -(double)n));
}
static double dan_present_value(double s, double r, double t) { return s / pow(1 + r / 100, t); }
static double dan_npv(double r, const dl_Lf *cf) {
    double s = 0, k = 1, q = 1 + r / 100;
    for (int64_t i = 0; i < cf->len; i++) { s += cf->items[i] / k; k *= q; }
    return s;
}
static do_f dan_irr(const dl_Lf *cf, int line) {
    (void)line;
    double lo = -99.999999, hi = 1e6, flo = dan_npv(lo, cf), fhi = dan_npv(hi, cf);
    /* ищем отрезок со сменой знака на сетке, затем делим пополам */
    double prev = lo, fprev = flo;
    bool found = false;
    for (double x = -99.0; x <= hi; x = x < 100 ? x + 1 : x * 1.1) {
        double fx = dan_npv(x, cf);
        if (fprev == 0) { return do_f_some(prev); }
        if ((fprev < 0) != (fx < 0)) { lo = prev; hi = x; flo = fprev; fhi = fx; found = true; break; }
        prev = x; fprev = fx;
    }
    if (!found) return do_f_none();
    for (int it = 0; it < 200; it++) {
        double mid = (lo + hi) / 2, fm = dan_npv(mid, cf);
        if ((flo < 0) == (fm < 0)) { lo = mid; flo = fm; } else { hi = mid; fhi = fm; }
    }
    (void)fhi;
    return do_f_some((lo + hi) / 2);
}
static double dan_real_rate(double nominal, double inflation) { return ((1 + nominal / 100) / (1 + inflation / 100) - 1) * 100; }
static dan_rat dan_daily_interest(dan_rat s, dan_rat r, int64_t days, int64_t year, int line) {
    if (year <= 0 || days < 0) dan_error(DAN_MSG_YEAR_DAYS, line);
    return dan_rat_div(dan_rat_mul(dan_rat_mul(s, dan_pct_r(r, line), line), dan_rat_from_int(days, line), line),
                       dan_rat_from_int(year, line), line);
}
static dan_rat dan_late_pay_comp(dan_rat s, dan_rat key_rate, int64_t days, int line) {
    if (days < 0) dan_error(DAN_MSG_YEAR_DAYS, line);
    return dan_rat_div(dan_rat_mul(dan_rat_mul(s, dan_pct_r(key_rate, line), line), dan_rat_from_int(days, line), line),
                       dan_rat_from_int(150, line), line);
}

/* -- parsing numbers: errors as values -- */
static dan_str dan_not_number_msg(dan_str s, const char *what) {
    dan_buf b = { NULL, 0, 0 };
    dan_buf_add(&b, "\"");
    dan_buf_add(&b, s);
    dan_buf_add(&b, what);
    return dan_buf_str(&b);
}
static dr_i dan_parse_int_res(dan_str s, int line) {
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    errno = 0;
    char *end;
    long long v = strtoll(p, &end, 10);
    if (end == p) return dr_i_err(dan_not_number_msg(s, DAN_TXT_NOT_INT), line);
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if (*end) return dr_i_err(dan_not_number_msg(s, DAN_TXT_NOT_INT), line);
    if (errno == ERANGE) return dr_i_err(dan_not_number_msg(s, DAN_TXT_INT_BIG), line);
    return dr_i_some((int64_t)v);
}
static dr_f dan_parse_float_res(dan_str s, int line) {
    size_t n = strlen(s);
    char *t = dan_alloc(n + 1);
    for (size_t i = 0; i <= n; i++) t[i] = s[i] == ',' ? '.' : s[i];
    const char *p = t;
    while (*p == ' ' || *p == '\t') p++;
    char *end;
    double v = strtod(p, &end);
    bool bad = end == p;
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') end++;
    if (*end) bad = true;
    free(t);
    if (bad) return dr_f_err(dan_not_number_msg(s, DAN_TXT_NOT_FLOAT), line);
    return dr_f_some(v);
}

/* -- logic -- */
static bool dan_implies(bool a, bool b) { return !a || b; }
static bool dan_equiv(bool a, bool b) { return a == b; }
static bool dan_xor(bool a, bool b) { return a != b; }

/* -- linear programming: max c·x, A·x <= b, x >= 0, b >= 0; exact simplex with Bland's rule -- */
static do_Lr dan_lp(const dl_Lr *c, const dl_LLr *a, const dl_Lr *b, bool minimize, int line) {
    int64_t n = c->len, m = a->len;
    if (n == 0 || m != b->len) dan_error(DAN_MSG_LP_DIMS, line);
    for (int64_t i = 0; i < m; i++) {
        if (a->items[i]->len != n) dan_error(DAN_MSG_LP_DIMS, line);
        if (b->items[i].n < 0) dan_error(DAN_MSG_LP_RHS, line);
    }
    int64_t w = n + m + 1;                      /* столбцы: x, дополнительные, правая часть */
    dan_rat *t = dan_alloc(sizeof(dan_rat) * (size_t)((m + 1) * w));
    int64_t *basis = dan_alloc(sizeof(int64_t) * (size_t)(m ? m : 1));
    dan_rat zero = { 0, 1 }, one = { 1, 1 };
    for (int64_t i = 0; i < (m + 1) * w; i++) t[i] = zero;
    for (int64_t i = 0; i < m; i++) {
        for (int64_t j = 0; j < n; j++) t[i * w + j] = a->items[i]->items[j];
        t[i * w + n + i] = one;
        t[i * w + w - 1] = b->items[i];
        basis[i] = n + i;
    }
    for (int64_t j = 0; j < n; j++) t[m * w + j] = minimize ? c->items[j] : dan_rat_neg(c->items[j], line);
    for (;;) {
        int64_t col = -1;
        for (int64_t j = 0; j < w - 1; j++) if (t[m * w + j].n < 0) { col = j; break; }   /* правило Бленда */
        if (col < 0) break;
        int64_t row = -1;
        dan_rat best = zero;
        for (int64_t i = 0; i < m; i++) {
            if (t[i * w + col].n <= 0) continue;
            dan_rat ratio = dan_rat_div(t[i * w + w - 1], t[i * w + col], line);
            if (row < 0 || dan_rat_cmp(ratio, best) < 0 || (dan_rat_cmp(ratio, best) == 0 && basis[i] < basis[row])) {
                row = i; best = ratio;
            }
        }
        if (row < 0) return do_Lr_none();                /* целевая функция не ограничена */
        dan_rat pv = t[row * w + col];
        for (int64_t j = 0; j < w; j++) t[row * w + j] = dan_rat_div(t[row * w + j], pv, line);
        for (int64_t i = 0; i <= m; i++) {
            if (i == row || t[i * w + col].n == 0) continue;
            dan_rat f = t[i * w + col];
            for (int64_t j = 0; j < w; j++) t[i * w + j] = dan_rat_sub(t[i * w + j], dan_rat_mul(f, t[row * w + j], line), line);
        }
        basis[row] = col;
    }
    dl_Lr *x = dl_Lr_from(0, NULL);
    for (int64_t j = 0; j < n; j++) dl_Lr_push(x, zero);
    for (int64_t i = 0; i < m; i++) if (basis[i] < n) x->items[basis[i]] = t[i * w + w - 1];
    return do_Lr_some(x);
}
static do_Lr dan_lp_max(const dl_Lr *c, const dl_LLr *a, const dl_Lr *b, int line) { return dan_lp(c, a, b, false, line); }
static do_Lr dan_lp_min(const dl_Lr *c, const dl_LLr *a, const dl_Lr *b, int line) { return dan_lp(c, a, b, true, line); }
"""
