#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct DialogField {
    enum class Kind {
        Text,            // произвольный текст, пустой допустим
        Amount,          // сумма со знаком, ноль запрещён
        AmountPositive,  // сумма без знака (начальный долг)
        Date,            // "ГГГГ-ММ-ДД"; пустая строка допустима
        Calendar,        // то же, но выбор мышью в календаре; пусто = «без даты»
    };

    std::wstring label;
    std::wstring hint;   // подсказка под полем (можно пустую)
    std::wstring value;  // на входе — начальное значение, на выходе — введённое
    Kind kind = Kind::Text;
};

struct DialogSpec {
    std::wstring title;
    std::wstring okLabel = L"Сохранить";
    std::vector<DialogField> fields;
};

// Модальный ввод. Возвращает false, если пользователь отменил.
// owner — окно верхнего уровня, которое владеет диалогом (скрытое окно приложения);
// anchor — окно, по которому диалог центрируется и у которого берётся DPI, обычно виджет.
// Разделены потому, что виджет может быть дочерним окном рабочего стола: сделать его
// владельцем нельзя — владельцем стал бы сам рабочий стол.
bool ShowDialog(HWND owner, HWND anchor, DialogSpec& spec);
